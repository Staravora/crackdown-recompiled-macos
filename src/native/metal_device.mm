#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <vector>

#include "metal_device.h"

// A view whose own layer is the Metal one. Handing an ordinary content view a layer shows nothing.
@interface ShadowView : NSView
@property(nonatomic, strong) CAMetalLayer* metalLayer;
@end

@implementation ShadowView
- (CALayer*)makeBackingLayer {
  return self.metalLayer;
}
- (BOOL)wantsUpdateLayer {
  return YES;
}
@end

namespace crackdown::native {
namespace {

using Clock = std::chrono::steady_clock;

constexpr NSUInteger kPushConstants = 8;
constexpr NSUInteger kGuestBuffer = 9;
constexpr NSUInteger kFetchBuffer = 10;
constexpr NSUInteger kElementsBuffer = 11;
constexpr NSUInteger kDisplayWidth = 1280;
constexpr NSUInteger kDisplayHeight = 720;
constexpr size_t kElementsBytes = 1 << 20;
constexpr int kDescriptorHeaps = 4;

constexpr uint32_t kDepthEnable = 1 << 1;
constexpr uint32_t kDepthWrite = 1 << 2;
constexpr uint32_t kDepthFunctionShift = 4;

// Grey clay: light from the slope of the depth, since there are no normals or textures to go on yet.
const char* kClaySource = R"(
#include <metal_stdlib>
using namespace metal;
fragment float4 clay_fragment(float4 position [[position]]) {
  float distance = 1.0 / position.w;
  float3 normal = normalize(float3(-dfdx(distance), -dfdy(distance), 0.02));
  float light = 0.25 + 0.75 * saturate(dot(normal, normalize(float3(0.3, 0.6, 0.75))));
  return float4(float3(light), 1.0);
}
)";

struct PushConstants {
  uint64_t vertex;
  uint64_t pixel;
  uint64_t shared;
};

uint64_t Since(Clock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

MTLCompareFunction CompareFunction(uint32_t xenos) {
  // Xenos numbers these as Metal's in the same order: never, less, equal, less-equal, greater, not-equal,
  // greater-equal, always. Metal's enum runs never, less, equal, less-equal, greater, not-equal, greater-equal, always.
  return MTLCompareFunction(xenos & 7);
}

void WritePng(const std::filesystem::path& path, const void* pixels, size_t width, size_t height) {
  CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
  CGContextRef context = CGBitmapContextCreate(const_cast<void*>(pixels), width, height, 8, width * 4, space,
                                               CGBitmapInfo(kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst));
  CGImageRef image = CGBitmapContextCreateImage(context);
  NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
  CGImageDestinationRef destination =
      CGImageDestinationCreateWithURL((__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
  if (destination && image) {
    CGImageDestinationAddImage(destination, image, nullptr);
    CGImageDestinationFinalize(destination);
  }
  if (destination) {
    CFRelease(destination);
  }
  CGImageRelease(image);
  CGContextRelease(context);
  CGColorSpaceRelease(space);
}

class MetalDeviceImpl final : public MetalDevice {
 public:
  bool Initialize(uint8_t* guest_memory, size_t guest_bytes, size_t frame_bytes) {
    device_ = MTLCreateSystemDefaultDevice();
    if (!device_) {
      return false;
    }
    queue_ = [device_ newCommandQueue];
    guest_memory_ = guest_memory;
    const MTLResourceOptions untracked = MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked;
    guest_ = [device_ newBufferWithBytesNoCopy:guest_memory length:guest_bytes options:untracked deallocator:nil];
    for (size_t i = 0; i < frames_.size(); ++i) {
      frames_[i] = [device_ newBufferWithLength:frame_bytes options:untracked | MTLResourceCPUCacheModeWriteCombined];
    }
    elements_ = [device_ newBufferWithLength:kElementsBytes options:untracked];

    NSError* error = nil;
    id<MTLLibrary> library = [device_ newLibraryWithSource:@(kClaySource) options:nil error:&error];
    if (!library) {
      return false;
    }
    clay_ = [library newFunctionWithName:@"clay_fragment"];
    return CreateDescriptorHeaps() && (slots_ = dispatch_semaphore_create(kFramesInFlight)) != nil;
  }

  bool guest_memory_wrapped() const override { return guest_ != nil; }

  void ShowWindow() override {
    // AppKit and Core Animation are the main thread's: a layer set up on a thread with no run loop never reaches the
    // screen, and every frame presented to it is dropped. orderFront leaves the keyboard with the game.
    id<MTLDevice> device = device_;
    dispatch_async(dispatch_get_main_queue(), ^{
      CAMetalLayer* layer = [CAMetalLayer layer];
      layer.device = device;
      layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
      layer.framebufferOnly = NO;
      layer.drawableSize = CGSizeMake(kDisplayWidth, kDisplayHeight);
      NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(60, 60, 640, 360)
                                                     styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable |
                                                               NSWindowStyleMaskMiniaturizable
                                                       backing:NSBackingStoreBuffered
                                                         defer:NO];
      window.title = @"Crackdown: Metal shadow";
      window.releasedWhenClosed = NO;
      ShadowView* view = [[ShadowView alloc] initWithFrame:window.contentLayoutRect];
      view.metalLayer = layer;
      view.wantsLayer = YES;
      view.layerContentsPlacement = NSViewLayerContentsPlacementScaleProportionallyToFit;
      window.contentView = view;
      layer.contentsScale = window.backingScaleFactor;
      [window orderFront:nil];
      window_ = window;
      [CATransaction flush];
      layer_ = layer;
      layer_ready_.store(true, std::memory_order_release);
    });
  }

  int AddVertexShader(const std::filesystem::path& path, const MetalPullElement* elements, size_t count,
                      std::string& error) override {
    @autoreleasepool {
      const size_t bytes = count * sizeof(MetalPullElement);
      if (elements_used_ + bytes > kElementsBytes) {
        error = "no room for vertex elements";
        return -1;
      }
      NSError* failure = nil;
      id<MTLLibrary> library = [device_ newLibraryWithURL:[NSURL fileURLWithPath:@(path.c_str())] error:&failure];
      if (!library) {
        error = failure.localizedDescription.UTF8String;
        return -1;
      }
      MTLFunctionConstantValues* values = [MTLFunctionConstantValues new];
      id<MTLFunction> vertex = [library newFunctionWithName:@"shaderMain" constantValues:values error:&failure];
      if (!vertex) {
        error = failure.localizedDescription.UTF8String;
        return -1;
      }
      MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
      descriptor.vertexFunction = vertex;
      descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
      id<MTLRenderPipelineState> depth_only = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&failure];
      descriptor.fragmentFunction = clay_;
      descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
      id<MTLRenderPipelineState> pipeline = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&failure];
      if (!pipeline || !depth_only) {
        error = failure.localizedDescription.UTF8String;
        return -1;
      }
      std::memcpy(static_cast<uint8_t*>(elements_.contents) + elements_used_, elements, bytes);
      pipelines_.push_back({pipeline, depth_only, elements_used_});
      elements_used_ += bytes;
      return int(pipelines_.size()) - 1;
    }
  }

  MetalFrameTiming RecordFrame(const std::function<void(uint8_t* frame)>& encode,
                               const std::filesystem::path& capture) override {
    MetalFrameTiming timing;
    @autoreleasepool {
      const Clock::time_point wait_start = Clock::now();
      dispatch_semaphore_wait(slots_, DISPATCH_TIME_FOREVER);
      timing.wait_ns = Since(wait_start);

      const Clock::time_point encode_start = Clock::now();
      // A drawable has to outlive this autorelease pool, so the window's frames keep their references.
      CAMetalLayer* layer = layer_ready_.load(std::memory_order_acquire) ? layer_ : nil;
      id<MTLCommandBuffer> buffer = layer ? [queue_ commandBuffer] : [queue_ commandBufferWithUnretainedReferences];
      buffer_ = buffer;
      frame_ = frames_[slot_];
      slot_ = (slot_ + 1) % kFramesInFlight;
      frame_address_ = frame_.gpuAddress;
      shown_target_ = -1;
      encode(static_cast<uint8_t*>(frame_.contents));
      EndPass();
      buffer_ = nil;

      id<MTLTexture> shown = shown_target_ >= 0 ? targets_[size_t(shown_target_)].color : nil;
      const MTLSize size = MTLSizeMake(shown.width, shown.height, 1);
      id<MTLBuffer> readback = nil;
      if (!capture.empty() && shown) {
        readback = [device_ newBufferWithLength:shown.width * shown.height * 4 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> blit = [buffer blitCommandEncoder];
        [blit copyFromTexture:shown
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:size
                        toBuffer:readback
               destinationOffset:0
          destinationBytesPerRow:shown.width * 4
        destinationBytesPerImage:shown.width * shown.height * 4];
        [blit endEncoding];
      }

      if (id<CAMetalDrawable> drawable = shown ? [layer nextDrawable] : nil) {
        id<MTLBlitCommandEncoder> blit = [buffer blitCommandEncoder];
        [blit copyFromTexture:shown
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(std::min(shown.width, drawable.texture.width),
                                          std::min(shown.height, drawable.texture.height), 1)
                    toTexture:drawable.texture
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [buffer presentDrawable:drawable];
      }

      dispatch_semaphore_t slots = slots_;
      [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
        if (done.status == MTLCommandBufferStatusError) {
          errors_.fetch_add(1, std::memory_order_relaxed);
          last_error_ = done.error;
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
      if (readback) {
        [buffer waitUntilCompleted];
        WritePng(capture, readback.contents, shown.width, shown.height);
      }
    }
    return timing;
  }

  int AddTarget(uint32_t width, uint32_t height, bool has_color) override {
    Target target;
    if (has_color) {
      target.color = NewTarget(MTLPixelFormatBGRA8Unorm, width, height);
    }
    target.depth = NewTarget(MTLPixelFormatDepth32Float, width, height);
    targets_.push_back(target);
    return int(targets_.size()) - 1;
  }

  void BeginPass(int target_index, bool clear, double clear_depth) override {
    EndPass();
    const Target& target = targets_[size_t(target_index)];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (target.color) {
      pass.colorAttachments[0].texture = target.color;
      pass.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0.08, 0.1, 0.14, 1.0);
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    pass.depthAttachment.texture = target.depth;
    pass.depthAttachment.loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.depthAttachment.clearDepth = clear_depth;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    encoder_ = [buffer_ renderCommandEncoderWithDescriptor:pass];
    for (int i = 0; i < kDescriptorHeaps; ++i) {
      [encoder_ setVertexBuffer:heaps_[i] offset:0 atIndex:i];
    }
    [encoder_ setVertexBuffer:guest_ offset:0 atIndex:kGuestBuffer];
    [encoder_ setVertexBuffer:frame_ offset:0 atIndex:kFetchBuffer];
    [encoder_ setVertexBuffer:elements_ offset:0 atIndex:kElementsBuffer];
    [encoder_ useResource:dummy_ usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
    [encoder_ setCullMode:MTLCullModeNone];
    pass_has_color_ = target.color != nil;
    current_pipeline_ = -1;
    current_depth_ = UINT32_MAX;
    current_viewport_ = {};
  }

  void ShowTarget(int target) override { shown_target_ = target; }

  void Draw(const MetalDraw& draw) override {
    if (draw.vertex_shader != current_pipeline_) {
      const Pipeline& pipeline = pipelines_[size_t(draw.vertex_shader)];
      [encoder_ setRenderPipelineState:pass_has_color_ ? pipeline.state : pipeline.depth_only];
      [encoder_ setVertexBufferOffset:pipeline.elements atIndex:kElementsBuffer];
      current_pipeline_ = draw.vertex_shader;
    }
    if (!(draw.viewport == current_viewport_)) {
      [encoder_ setViewport:{draw.viewport.x, draw.viewport.y, draw.viewport.width, draw.viewport.height, 0.0, 1.0}];
      current_viewport_ = draw.viewport;
    }
    if (draw.depth_control != current_depth_) {
      [encoder_ setDepthStencilState:DepthState(draw.depth_control)];
      current_depth_ = draw.depth_control;
    }
    const PushConstants push = {frame_address_ + draw.vertex_constants, frame_address_ + draw.vertex_constants,
                                frame_address_ + draw.shared_constants};
    [encoder_ setVertexBytes:&push length:sizeof(push) atIndex:kPushConstants];
    [encoder_ setVertexBufferOffset:draw.fetch_constants atIndex:kFetchBuffer];
    const MTLPrimitiveType type =
        draw.primitive == MetalPrimitive::kTriangleStrip ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle;
    [encoder_ drawIndexedPrimitives:type
                         indexCount:draw.index_count
                          indexType:draw.wide_indices ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                        indexBuffer:frame_
                  indexBufferOffset:draw.indices
                      instanceCount:1
                         baseVertex:draw.base_vertex
                       baseInstance:0];
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
  struct Pipeline {
    id<MTLRenderPipelineState> state;
    id<MTLRenderPipelineState> depth_only;
    size_t elements;
  };

  struct Target {
    id<MTLTexture> color;
    id<MTLTexture> depth;
  };

  id<MTLTexture> NewTarget(MTLPixelFormat format, uint32_t width, uint32_t height) {
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                          width:width
                                                                                         height:height
                                                                                      mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget;
    descriptor.storageMode = MTLStorageModePrivate;
    return [device_ newTextureWithDescriptor:descriptor];
  }

  void EndPass() {
    [encoder_ endEncoding];
    encoder_ = nil;
  }

  // The shaders index tables of resource IDs. Until textures arrive every slot holds one dummy, because a few vertex
  // shaders read a map and an empty table would fault the GPU.
  bool CreateDescriptorHeaps() {
    MTLTextureDescriptor* two_d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:1
                                                                                    height:1
                                                                                 mipmapped:NO];
    two_d.storageMode = MTLStorageModeShared;
    dummy_ = [device_ newTextureWithDescriptor:two_d];
    MTLTextureDescriptor* three_d = [MTLTextureDescriptor new];
    three_d.textureType = MTLTextureType3D;
    three_d.pixelFormat = MTLPixelFormatRGBA8Unorm;
    three_d.storageMode = MTLStorageModeShared;
    dummy_3d_ = [device_ newTextureWithDescriptor:three_d];
    MTLTextureDescriptor* cube = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                       size:1
                                                                                  mipmapped:NO];
    cube.storageMode = MTLStorageModeShared;
    dummy_cube_ = [device_ newTextureWithDescriptor:cube];
    MTLSamplerDescriptor* sampler = [MTLSamplerDescriptor new];
    sampler.supportArgumentBuffers = YES;
    sampler_ = [device_ newSamplerStateWithDescriptor:sampler];
    const std::array<MTLResourceID, kDescriptorHeaps> ids = {dummy_.gpuResourceID, dummy_3d_.gpuResourceID,
                                                             dummy_cube_.gpuResourceID, sampler_.gpuResourceID};
    for (int i = 0; i < kDescriptorHeaps; ++i) {
      heaps_[i] = [device_ newBufferWithLength:sizeof(MTLResourceID) * 64 options:MTLResourceStorageModeShared];
      auto* slots = static_cast<MTLResourceID*>(heaps_[i].contents);
      std::fill_n(slots, 64, ids[size_t(i)]);
    }
    return true;
  }

  id<MTLDepthStencilState> DepthState(uint32_t control) {
    const uint32_t key = control & (kDepthEnable | kDepthWrite | (7u << kDepthFunctionShift));
    auto found = depth_states_.find(key);
    if (found != depth_states_.end()) {
      return found->second;
    }
    MTLDepthStencilDescriptor* descriptor = [MTLDepthStencilDescriptor new];
    descriptor.depthCompareFunction =
        (key & kDepthEnable) ? CompareFunction(key >> kDepthFunctionShift) : MTLCompareFunctionAlways;
    descriptor.depthWriteEnabled = (key & kDepthEnable) && (key & kDepthWrite);
    id<MTLDepthStencilState> state = [device_ newDepthStencilStateWithDescriptor:descriptor];
    depth_states_[key] = state;
    return state;
  }

  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLBuffer> guest_ = nil;
  id<MTLBuffer> elements_ = nil;
  id<MTLBuffer> frame_ = nil;
  id<MTLTexture> dummy_ = nil;
  id<MTLTexture> dummy_3d_ = nil;
  id<MTLTexture> dummy_cube_ = nil;
  id<MTLSamplerState> sampler_ = nil;
  id<MTLFunction> clay_ = nil;
  id<MTLCommandBuffer> buffer_ = nil;
  id<MTLRenderCommandEncoder> encoder_ = nil;
  CAMetalLayer* layer_ = nil;
  NSWindow* window_ = nil;
  NSError* last_error_ = nil;
  std::array<id<MTLBuffer>, kFramesInFlight> frames_{};
  std::array<id<MTLBuffer>, kDescriptorHeaps> heaps_{};
  std::vector<Pipeline> pipelines_;
  std::vector<Target> targets_;
  std::map<uint32_t, id<MTLDepthStencilState>> depth_states_;
  dispatch_semaphore_t slots_ = nil;
  uint8_t* guest_memory_ = nullptr;
  uint64_t frame_address_ = 0;
  size_t elements_used_ = 0;
  uint32_t slot_ = 0;
  int current_pipeline_ = -1;
  int shown_target_ = -1;
  bool pass_has_color_ = false;
  MetalViewport current_viewport_;
  uint32_t current_depth_ = UINT32_MAX;
  std::atomic<bool> layer_ready_{false};
  std::atomic<uint64_t> completed_{0};
  std::atomic<uint64_t> errors_{0};
  std::atomic<uint64_t> gpu_ns_{0};
  std::atomic<uint64_t> worst_gpu_ns_{0};
};

}  // namespace

std::unique_ptr<MetalDevice> MetalDevice::Create(uint8_t* guest_memory, size_t guest_bytes, size_t frame_bytes) {
  auto device = std::make_unique<MetalDeviceImpl>();
  @autoreleasepool {
    if (!device->Initialize(guest_memory, guest_bytes, frame_bytes)) {
      return nullptr;
    }
  }
  return device;
}

}  // namespace crackdown::native
