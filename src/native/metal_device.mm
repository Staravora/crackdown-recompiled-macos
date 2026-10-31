#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>

#include "metal_device.h"

namespace crackdown::native {
namespace {

using Clock = std::chrono::steady_clock;

constexpr NSUInteger kGuestBuffer = 0;
constexpr NSUInteger kConstantBuffer = 1;
constexpr NSUInteger kTargetWidth = 1280;
constexpr NSUInteger kTargetHeight = 720;

// Every vertex lands outside the clip volume, so these draws cost recording and no pixels.
const char* kPlaceholderSource = R"(
#include <metal_stdlib>
using namespace metal;
constant int kVariant [[function_constant(0)]];
struct VertexOut { float4 position [[position]]; };
vertex VertexOut placeholder_vertex(constant float4* constants [[buffer(1)]]) {
  VertexOut out;
  out.position = float4(2.0, 2.0, 2.0, 1.0) + constants[0] * 0.0;
  return out;
}
fragment float4 placeholder_fragment(constant float4* constants [[buffer(1)]]) {
  return constants[0] * (1.0 + float(kVariant) * 0.0001);
}
)";

uint64_t Since(Clock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

class MetalDeviceImpl final : public MetalDevice {
 public:
  bool Initialize(uint8_t* guest_memory, size_t guest_bytes, size_t constant_bytes) {
    device_ = MTLCreateSystemDefaultDevice();
    if (!device_) {
      return false;
    }
    queue_ = [device_ newCommandQueue];
    guest_memory_ = guest_memory;
    guest_ = [device_ newBufferWithBytesNoCopy:guest_memory
                                        length:guest_bytes
                                       options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked
                                   deallocator:nil];
    for (size_t i = 0; i < constants_.size(); ++i) {
      constants_[i] = [device_ newBufferWithLength:constant_bytes
                                           options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined |
                                                   MTLResourceHazardTrackingModeUntracked];
    }

    MTLTextureDescriptor* target = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:kTargetWidth
                                                                                     height:kTargetHeight
                                                                                  mipmapped:NO];
    target.usage = MTLTextureUsageRenderTarget;
    target.storageMode = MTLStorageModePrivate;
    target_ = [device_ newTextureWithDescriptor:target];

    NSError* error = nil;
    id<MTLLibrary> library = [device_ newLibraryWithSource:@(kPlaceholderSource) options:nil error:&error];
    if (!library) {
      return false;
    }
    id<MTLFunction> vertex = [library newFunctionWithName:@"placeholder_vertex"];
    for (uint32_t i = 0; i < kPipelines; ++i) {
      int variant = int(i);
      MTLFunctionConstantValues* values = [MTLFunctionConstantValues new];
      [values setConstantValue:&variant type:MTLDataTypeInt atIndex:0];
      id<MTLFunction> fragment = [library newFunctionWithName:@"placeholder_fragment" constantValues:values error:&error];
      if (!fragment) {
        return false;
      }
      MTLRenderPipelineDescriptor* pipeline = [MTLRenderPipelineDescriptor new];
      pipeline.vertexFunction = vertex;
      pipeline.fragmentFunction = fragment;
      pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
      pipelines_[i] = [device_ newRenderPipelineStateWithDescriptor:pipeline error:&error];
      if (!pipelines_[i]) {
        return false;
      }
    }
    slots_ = dispatch_semaphore_create(kFramesInFlight);
    return true;
  }

  bool guest_memory_wrapped() const override { return guest_ != nil; }

  MetalFrameTiming RecordFrame(const std::function<void(uint8_t* constants)>& encode) override {
    MetalFrameTiming timing;
    @autoreleasepool {
      const Clock::time_point wait_start = Clock::now();
      dispatch_semaphore_wait(slots_, DISPATCH_TIME_FOREVER);
      timing.wait_ns = Since(wait_start);

      const Clock::time_point encode_start = Clock::now();
      id<MTLCommandBuffer> buffer = [queue_ commandBufferWithUnretainedReferences];
      MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = target_;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionDontCare;
      encoder_ = [buffer renderCommandEncoderWithDescriptor:pass];

      id<MTLBuffer> constants = constants_[slot_];
      slot_ = (slot_ + 1) % kFramesInFlight;
      [encoder_ setVertexBuffer:guest_ offset:0 atIndex:kGuestBuffer];
      [encoder_ setVertexBuffer:constants offset:0 atIndex:kConstantBuffer];
      [encoder_ setFragmentBuffer:constants offset:0 atIndex:kConstantBuffer];
      current_pipeline_ = UINT32_MAX;
      encode(static_cast<uint8_t*>(constants.contents));
      [encoder_ endEncoding];
      encoder_ = nil;

      dispatch_semaphore_t slots = slots_;
      [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
        if (done.status == MTLCommandBufferStatusError) {
          errors_.fetch_add(1, std::memory_order_relaxed);
        }
        const uint64_t gpu_ns = uint64_t(std::max(0.0, done.GPUEndTime - done.GPUStartTime) * 1e9);
        gpu_ns_.fetch_add(gpu_ns, std::memory_order_relaxed);
        uint64_t worst = worst_gpu_ns_.load(std::memory_order_relaxed);
        while (worst < gpu_ns && !worst_gpu_ns_.compare_exchange_weak(worst, gpu_ns, std::memory_order_relaxed)) {
        }
        completed_.fetch_add(1, std::memory_order_relaxed);
        dispatch_semaphore_signal(slots);
      }];
      [buffer commit];
      timing.encode_ns = Since(encode_start);
    }
    return timing;
  }

  void Draw(uint32_t pipeline, size_t vertex_constants, size_t fragment_constants) override {
    if (pipeline != current_pipeline_) {
      [encoder_ setRenderPipelineState:pipelines_[pipeline]];
      current_pipeline_ = pipeline;
    }
    [encoder_ setVertexBufferOffset:vertex_constants atIndex:kConstantBuffer];
    [encoder_ setFragmentBufferOffset:fragment_constants atIndex:kConstantBuffer];
    [encoder_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }

  bool MatchesGuestMemory(uint32_t address, uint32_t bytes) override {
    if (!guest_) {
      return false;
    }
    @autoreleasepool {
      id<MTLBuffer> copy = [device_ newBufferWithLength:bytes options:MTLResourceStorageModeShared];
      id<MTLCommandBuffer> buffer = [queue_ commandBuffer];
      id<MTLBlitCommandEncoder> blit = [buffer blitCommandEncoder];
      [blit copyFromBuffer:guest_ sourceOffset:address toBuffer:copy destinationOffset:0 size:bytes];
      [blit endEncoding];
      [buffer commit];
      [buffer waitUntilCompleted];
      return buffer.status == MTLCommandBufferStatusCompleted &&
             std::memcmp(copy.contents, guest_memory_ + address, bytes) == 0;
    }
  }

  MetalGpuTotals gpu_totals() const override {
    return {completed_.load(std::memory_order_relaxed), errors_.load(std::memory_order_relaxed),
            gpu_ns_.load(std::memory_order_relaxed), worst_gpu_ns_.load(std::memory_order_relaxed)};
  }

 private:
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLBuffer> guest_ = nil;
  id<MTLTexture> target_ = nil;
  id<MTLRenderCommandEncoder> encoder_ = nil;
  std::array<id<MTLBuffer>, kFramesInFlight> constants_{};
  std::array<id<MTLRenderPipelineState>, kPipelines> pipelines_{};
  dispatch_semaphore_t slots_ = nil;
  uint8_t* guest_memory_ = nullptr;
  uint32_t slot_ = 0;
  uint32_t current_pipeline_ = UINT32_MAX;
  std::atomic<uint64_t> completed_{0};
  std::atomic<uint64_t> errors_{0};
  std::atomic<uint64_t> gpu_ns_{0};
  std::atomic<uint64_t> worst_gpu_ns_{0};
};

}  // namespace

std::unique_ptr<MetalDevice> MetalDevice::Create(uint8_t* guest_memory, size_t guest_bytes, size_t constant_bytes) {
  auto device = std::make_unique<MetalDeviceImpl>();
  @autoreleasepool {
    if (!device->Initialize(guest_memory, guest_bytes, constant_bytes)) {
      return nullptr;
    }
  }
  return device;
}

}  // namespace crackdown::native
