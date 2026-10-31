// How fast can this Mac record a Crackdown frame in Metal? Standalone: no game code, no window.
// Frame shape comes from the census (docs/native-rendering-survey.md): 4,700 draws, 24 passes, 391
// pipelines, about 200 textures. Per-draw state changes the census did not count are pessimistic
// guesses, listed in kPipelineChange and friends. Only Metal recording is timed, not the work a real
// layer does to decide what to record.
//
// Build: clang++ -std=c++20 -O2 -fobjc-arc tools/encode_bench.mm -framework Metal -framework Foundation -o encode_bench
// Run:   ./encode_bench [frames per run] [rounds] [stress]

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <pthread/qos.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

int kDraws = 4700;
constexpr int kPasses = 24;
constexpr int kPipelines = 391;
constexpr int kTextures = 200;
constexpr int kDepthStates = 8;
double kPipelineChange = 0.5;
double kTextureChange = 0.6;
double kSecondTexture = 0.2;
double kDepthChange = 0.15;
double kScissorChange = 0.1;
// Share of draws in the game's baked command buffers: 41-64% in the census.
constexpr double kBakedShare = 0.64;
constexpr size_t kVertexConstants = 4096;
constexpr size_t kFragmentConstants = 1024;
constexpr size_t kAlign = 16;
constexpr int kFramesInFlight = 3;
constexpr int kTrianglesPerDraw = 12;
constexpr double kFrameMs = 1000.0 / 30.0;

const char* kSource = R"(
#include <metal_stdlib>
using namespace metal;
constant int kVariant [[function_constant(0)]];
struct VertexOut { float4 position [[position]]; float2 uv; uint first; uint second; };
struct VertexConstants { float4 transform; uint first; uint second; float4 padding[62]; };
struct FragmentConstants { float4 tint; float4 padding[15]; };
vertex VertexOut vertex_main(uint id [[vertex_id]], const device packed_float2* positions [[buffer(0)]],
                             constant VertexConstants& c [[buffer(1)]]) {
  VertexOut out;
  float2 p = positions[id];
  out.position = float4(p * c.transform.xy + c.transform.zw, 0.5, 1.0);
  out.uv = p;
  out.first = c.first;
  out.second = c.second;
  return out;
}
fragment float4 fragment_bound(VertexOut in [[stage_in]], texture2d<float> first [[texture(0)]],
                               texture2d<float> second [[texture(1)]], sampler s [[sampler(0)]],
                               constant FragmentConstants& c [[buffer(0)]]) {
  return first.sample(s, in.uv) * second.sample(s, in.uv) * c.tint * (1.0 + float(kVariant) * 0.0001);
}
struct Textures { array<texture2d<float>, 256> t [[id(0)]]; };
fragment float4 fragment_bindless(VertexOut in [[stage_in]], constant Textures& textures [[buffer(1)]],
                                  constant FragmentConstants& c [[buffer(0)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  return textures.t[in.first].sample(s, in.uv) * textures.t[in.second].sample(s, in.uv) * c.tint *
         (1.0 + float(kVariant) * 0.0001);
}
)";

struct Draw {
  uint16_t pipeline;
  uint16_t first_texture;
  uint16_t second_texture;
  uint16_t depth_state;
  uint32_t vertex_offset;
  uint32_t index_offset;
  uint16_t scissor_x, scissor_y;
  float transform[4];
  bool change_pipeline, change_texture, change_second, change_depth, change_scissor;
  bool baked;
};

struct Pass {
  int first;
  int count;
};

struct Scenario {
  const char* name;
  bool bindless;
  int threads;
  bool indirect;
};

double NowMs() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

double Percentile(std::vector<double> values, double fraction) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  return values[std::min(values.size() - 1, size_t(values.size() * fraction))];
}

class Bench {
 public:
  bool Setup();
  void RunScenario(const Scenario& scenario, int frames, std::vector<double>& cpu, std::vector<double>& gpu);

 private:
  void EncodeRange(id<MTLRenderCommandEncoder> encoder, const Scenario& scenario, int first, int count,
                   uint8_t* ring, size_t& ring_offset, id<MTLBuffer> ring_buffer);
  void PrepareEncoder(id<MTLRenderCommandEncoder> encoder, const Scenario& scenario);
  void EncodeFrame(id<MTLCommandBuffer> command_buffer, const Scenario& scenario, int frame);
  bool BuildIndirect();

  id<MTLDevice> device_;
  id<MTLCommandQueue> queue_;
  std::vector<id<MTLRenderPipelineState>> bound_pipelines_, bindless_pipelines_;
  std::vector<id<MTLDepthStencilState>> depth_states_;
  std::vector<id<MTLTexture>> textures_;
  id<MTLHeap> heap_;
  id<MTLSamplerState> sampler_;
  id<MTLBuffer> vertices_, indices_, texture_table_, baked_constants_;
  id<MTLBuffer> rings_[kFramesInFlight];
  id<MTLTexture> colour_, depth_;
  id<MTLIndirectCommandBuffer> indirect_;
  std::vector<Draw> draws_;
  std::vector<Pass> passes_;
  std::vector<std::pair<int, int>> indirect_ranges_;
  uint8_t vertex_source_[kVertexConstants];
  uint8_t fragment_source_[kFragmentConstants];
};

bool Bench::Setup() {
  device_ = MTLCreateSystemDefaultDevice();
  queue_ = [device_ newCommandQueue];
  NSError* error = nil;
  id<MTLLibrary> library = [device_ newLibraryWithSource:@(kSource) options:nil error:&error];
  if (!library) {
    fprintf(stderr, "library: %s\n", error.localizedDescription.UTF8String);
    return false;
  }

  id<MTLFunction> vertex = [library newFunctionWithName:@"vertex_main"
                                         constantValues:[[MTLFunctionConstantValues alloc] init]
                                                  error:nil];
  double compile_start = NowMs();
  for (int i = 0; i < kPipelines; ++i) {
    MTLFunctionConstantValues* values = [[MTLFunctionConstantValues alloc] init];
    int variant = i;
    [values setConstantValue:&variant type:MTLDataTypeInt atIndex:0];
    for (int bindless = 0; bindless < 2; ++bindless) {
      id<MTLFunction> fragment =
          [library newFunctionWithName:bindless ? @"fragment_bindless" : @"fragment_bound"
                        constantValues:values
                                 error:&error];
      MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
      descriptor.vertexFunction = vertex;
      descriptor.fragmentFunction = fragment;
      descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
      descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
      descriptor.supportIndirectCommandBuffers = bindless;
      id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&error];
      if (!state) {
        fprintf(stderr, "pipeline: %s\n", error.localizedDescription.UTF8String);
        return false;
      }
      (bindless ? bindless_pipelines_ : bound_pipelines_).push_back(state);
    }
  }
  printf("%d pipeline pairs compiled in %.0f ms\n", kPipelines, NowMs() - compile_start);

  for (int i = 0; i < kDepthStates; ++i) {
    MTLDepthStencilDescriptor* descriptor = [[MTLDepthStencilDescriptor alloc] init];
    descriptor.depthCompareFunction = (i & 1) ? MTLCompareFunctionLessEqual : MTLCompareFunctionAlways;
    descriptor.depthWriteEnabled = (i & 2) != 0;
    descriptor.label = [NSString stringWithFormat:@"%d", i];
    depth_states_.push_back([device_ newDepthStencilStateWithDescriptor:descriptor]);
  }

  MTLTextureDescriptor* texture_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:256
                                                        height:256
                                                     mipmapped:NO];
  texture_descriptor.storageMode = MTLStorageModeShared;
  MTLSizeAndAlign size = [device_ heapTextureSizeAndAlignWithDescriptor:texture_descriptor];
  MTLHeapDescriptor* heap_descriptor = [[MTLHeapDescriptor alloc] init];
  heap_descriptor.size = (size.size + size.align) * kTextures;
  heap_descriptor.storageMode = MTLStorageModeShared;
  heap_ = [device_ newHeapWithDescriptor:heap_descriptor];
  std::vector<uint32_t> pixels(256 * 256);
  texture_table_ = [device_ newBufferWithLength:sizeof(MTLResourceID) * 256 options:MTLResourceStorageModeShared];
  auto* table = static_cast<MTLResourceID*>(texture_table_.contents);
  for (int i = 0; i < kTextures; ++i) {
    id<MTLTexture> texture = [heap_ newTextureWithDescriptor:texture_descriptor];
    std::fill(pixels.begin(), pixels.end(), 0xFF000000u | uint32_t(i * 0x010203u));
    [texture replaceRegion:MTLRegionMake2D(0, 0, 256, 256) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:1024];
    textures_.push_back(texture);
    table[i] = texture.gpuResourceID;
  }
  for (int i = kTextures; i < 256; ++i) table[i] = textures_[0].gpuResourceID;

  MTLSamplerDescriptor* sampler = [[MTLSamplerDescriptor alloc] init];
  sampler.minFilter = MTLSamplerMinMagFilterLinear;
  sampler.magFilter = MTLSamplerMinMagFilterLinear;
  sampler.supportArgumentBuffers = YES;
  sampler_ = [device_ newSamplerStateWithDescriptor:sampler];

  constexpr int kVertices = 4096;
  vertices_ = [device_ newBufferWithLength:sizeof(float) * 2 * kVertices options:MTLResourceStorageModeShared];
  indices_ = [device_ newBufferWithLength:sizeof(uint16_t) * kVertices * 4 options:MTLResourceStorageModeShared];
  std::mt19937 random(1234);
  std::uniform_real_distribution<float> unit(0.0f, 1.0f);
  auto* positions = static_cast<float*>(vertices_.contents);
  for (int i = 0; i < kVertices * 2; ++i) positions[i] = unit(random);
  auto* index_data = static_cast<uint16_t*>(indices_.contents);
  for (int i = 0; i < kVertices * 4; ++i) index_data[i] = uint16_t(random() % 512);

  for (int i = 0; i < kFramesInFlight; ++i) {
    rings_[i] = [device_ newBufferWithLength:48 * 1024 * 1024 options:MTLResourceStorageModeShared];
  }
  for (size_t i = 0; i < sizeof(vertex_source_); ++i) vertex_source_[i] = uint8_t(i * 7);
  for (size_t i = 0; i < sizeof(fragment_source_); ++i) fragment_source_[i] = uint8_t(i * 3);

  MTLTextureDescriptor* target =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:1280
                                                        height:720
                                                     mipmapped:NO];
  target.usage = MTLTextureUsageRenderTarget;
  target.storageMode = MTLStorageModePrivate;
  colour_ = [device_ newTextureWithDescriptor:target];
  target.pixelFormat = MTLPixelFormatDepth32Float;
  depth_ = [device_ newTextureWithDescriptor:target];

  // The main view and a shadow pass carry most draws; the rest split across the post passes.
  int counts[kPasses] = {2700, 1000, 400};
  for (int i = 3; i < kPasses; ++i) counts[i] = (kDraws - 4100) / (kPasses - 3);
  counts[kPasses - 1] += kDraws - 4100 - (kDraws - 4100) / (kPasses - 3) * (kPasses - 3);
  int first = 0;
  for (int count : counts) {
    passes_.push_back({first, count});
    first += count;
  }

  std::bernoulli_distribution pipeline_change(kPipelineChange), texture_change(kTextureChange),
      second_texture(kSecondTexture), depth_change(kDepthChange), scissor_change(kScissorChange), baked(kBakedShare);
  draws_.resize(kDraws);
  for (Draw& draw : draws_) {
    draw.pipeline = uint16_t(random() % kPipelines);
    draw.first_texture = uint16_t(random() % kTextures);
    draw.second_texture = uint16_t(random() % kTextures);
    draw.depth_state = uint16_t(random() % kDepthStates);
    draw.vertex_offset = uint32_t(random() % 1024) * 8;
    draw.index_offset = uint32_t(random() % 1024) * 2;
    draw.scissor_x = uint16_t(random() % 640);
    draw.scissor_y = uint16_t(random() % 360);
    draw.transform[0] = 0.05f + unit(random) * 0.03f;
    draw.transform[1] = 0.05f + unit(random) * 0.03f;
    draw.transform[2] = unit(random) * 2.0f - 1.0f;
    draw.transform[3] = unit(random) * 2.0f - 1.0f;
    draw.change_pipeline = pipeline_change(random);
    draw.change_texture = texture_change(random);
    draw.change_second = second_texture(random);
    draw.change_depth = depth_change(random);
    draw.change_scissor = scissor_change(random);
    draw.baked = baked(random);
  }
  // Every run of a pass starts from known state.
  for (const Pass& pass : passes_) {
    Draw& head = draws_[pass.first];
    head.change_pipeline = head.change_texture = head.change_second = head.change_depth = true;
  }
  return BuildIndirect();
}

// The census found no baked buffer ever changing, so those draws could be recorded once.
bool Bench::BuildIndirect() {
  int baked_count = 0;
  for (const Draw& draw : draws_) baked_count += draw.baked;
  MTLIndirectCommandBufferDescriptor* descriptor = [[MTLIndirectCommandBufferDescriptor alloc] init];
  descriptor.commandTypes = MTLIndirectCommandTypeDrawIndexed;
  descriptor.inheritPipelineState = NO;
  descriptor.inheritBuffers = NO;
  descriptor.maxVertexBufferBindCount = 2;
  descriptor.maxFragmentBufferBindCount = 2;
  indirect_ = [device_ newIndirectCommandBufferWithDescriptor:descriptor maxCommandCount:baked_count options:0];
  if (!indirect_) return false;
  baked_constants_ = [device_ newBufferWithLength:size_t(baked_count) * (kVertexConstants + kFragmentConstants)
                                          options:MTLResourceStorageModeShared];
  auto* constants = static_cast<uint8_t*>(baked_constants_.contents);
  size_t offset = 0;
  int command = 0;
  for (const Pass& pass : passes_) {
    int start = command;
    for (int i = pass.first; i < pass.first + pass.count; ++i) {
      const Draw& draw = draws_[i];
      if (!draw.baked) continue;
      std::memcpy(constants + offset, vertex_source_, kVertexConstants);
      std::memcpy(constants + offset, draw.transform, sizeof(draw.transform));
      uint32_t ids[2] = {draw.first_texture, draw.second_texture};
      std::memcpy(constants + offset + sizeof(draw.transform), ids, sizeof(ids));
      std::memcpy(constants + offset + kVertexConstants, fragment_source_, kFragmentConstants);
      id<MTLIndirectRenderCommand> render = [indirect_ indirectRenderCommandAtIndex:command];
      [render setRenderPipelineState:bindless_pipelines_[draw.pipeline]];
      [render setVertexBuffer:vertices_ offset:draw.vertex_offset atIndex:0];
      [render setVertexBuffer:baked_constants_ offset:offset atIndex:1];
      [render setFragmentBuffer:baked_constants_ offset:offset + kVertexConstants atIndex:0];
      [render setFragmentBuffer:texture_table_ offset:0 atIndex:1];
      [render drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                         indexCount:kTrianglesPerDraw * 3
                          indexType:MTLIndexTypeUInt16
                        indexBuffer:indices_
                  indexBufferOffset:draw.index_offset
                      instanceCount:1
                         baseVertex:0
                       baseInstance:0];
      offset += kVertexConstants + kFragmentConstants;
      ++command;
    }
    indirect_ranges_.push_back({start, command - start});
  }
  return true;
}

void Bench::PrepareEncoder(id<MTLRenderCommandEncoder> encoder, const Scenario& scenario) {
  [encoder setVertexBuffer:vertices_ offset:0 atIndex:0];
  [encoder setFragmentSamplerState:sampler_ atIndex:0];
  if (scenario.bindless) {
    [encoder setFragmentBuffer:texture_table_ offset:0 atIndex:1];
    [encoder useHeap:heap_ stages:MTLRenderStageFragment];
  }
  [encoder setRenderPipelineState:(scenario.bindless ? bindless_pipelines_ : bound_pipelines_)[0]];
  [encoder setDepthStencilState:depth_states_[0]];
}

void Bench::EncodeRange(id<MTLRenderCommandEncoder> encoder, const Scenario& scenario, int first, int count,
                        uint8_t* ring, size_t& ring_offset, id<MTLBuffer> ring_buffer) {
  const auto& pipelines = scenario.bindless ? bindless_pipelines_ : bound_pipelines_;
  // Parallel encoders start with nothing bound, so the first draw of each must set everything.
  bool first_draw = true;
  for (int i = first; i < first + count; ++i) {
    const Draw& draw = draws_[i];
    if (scenario.indirect && draw.baked) continue;
    if (draw.change_pipeline || first_draw) [encoder setRenderPipelineState:pipelines[draw.pipeline]];
    if (draw.change_depth || first_draw) [encoder setDepthStencilState:depth_states_[draw.depth_state]];
    if (draw.change_scissor) {
      [encoder setScissorRect:MTLScissorRect{draw.scissor_x, draw.scissor_y, 640, 360}];
    }
    if (!scenario.bindless) {
      if (draw.change_texture || first_draw) {
        [encoder setFragmentTexture:textures_[draw.first_texture] atIndex:0];
      }
      if (draw.change_second || first_draw) {
        [encoder setFragmentTexture:textures_[draw.second_texture] atIndex:1];
      }
    }
    size_t offset = (ring_offset + kAlign - 1) & ~(kAlign - 1);
    uint8_t* constants = ring + offset;
    std::memcpy(constants, vertex_source_, kVertexConstants);
    std::memcpy(constants, draw.transform, sizeof(draw.transform));
    uint32_t ids[2] = {draw.first_texture, draw.second_texture};
    std::memcpy(constants + sizeof(draw.transform), ids, sizeof(ids));
    std::memcpy(constants + kVertexConstants, fragment_source_, kFragmentConstants);
    ring_offset = offset + kVertexConstants + kFragmentConstants;
    if (first_draw) {
      [encoder setVertexBuffer:ring_buffer offset:offset atIndex:1];
      [encoder setFragmentBuffer:ring_buffer offset:offset + kVertexConstants atIndex:0];
    } else {
      [encoder setVertexBufferOffset:offset atIndex:1];
      [encoder setFragmentBufferOffset:offset + kVertexConstants atIndex:0];
    }
    [encoder setVertexBufferOffset:draw.vertex_offset atIndex:0];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:kTrianglesPerDraw * 3
                         indexType:MTLIndexTypeUInt16
                       indexBuffer:indices_
                 indexBufferOffset:draw.index_offset];
    first_draw = false;
  }
}

void Bench::EncodeFrame(id<MTLCommandBuffer> command_buffer, const Scenario& scenario, int frame) {
  id<MTLBuffer> ring_buffer = rings_[frame % kFramesInFlight];
  auto* ring = static_cast<uint8_t*>(ring_buffer.contents);
  size_t ring_offset = 0;
  for (size_t p = 0; p < passes_.size(); ++p) {
    const Pass& pass = passes_[p];
    MTLRenderPassDescriptor* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    descriptor.colorAttachments[0].texture = colour_;
    descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
    descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
    descriptor.depthAttachment.texture = depth_;
    descriptor.depthAttachment.loadAction = MTLLoadActionClear;
    descriptor.depthAttachment.storeAction = MTLStoreActionDontCare;
    descriptor.depthAttachment.clearDepth = 1.0;

    if (scenario.threads > 1 && pass.count > 300) {
      id<MTLParallelRenderCommandEncoder> parallel =
          [command_buffer parallelRenderCommandEncoderWithDescriptor:descriptor];
      const int threads = scenario.threads;
      std::vector<id<MTLRenderCommandEncoder>> encoders(threads);
      for (int t = 0; t < threads; ++t) encoders[t] = [parallel renderCommandEncoder];
      std::vector<size_t> offsets(threads);
      for (int t = 0; t < threads; ++t) {
        offsets[t] = ring_offset + (pass.count / threads + 1) * (kVertexConstants + kFragmentConstants + kAlign) * t;
      }
      ring_offset += (pass.count + threads) * (kVertexConstants + kFragmentConstants + kAlign);
      dispatch_apply(size_t(threads), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^(size_t t) {
        @autoreleasepool {
          int share = pass.count / threads;
          int first = pass.first + share * int(t);
          int count = t + 1 == size_t(threads) ? pass.first + pass.count - first : share;
          size_t local_offset = offsets[t];
          PrepareEncoder(encoders[t], scenario);
          EncodeRange(encoders[t], scenario, first, count, ring, local_offset, ring_buffer);
          [encoders[t] endEncoding];
        }
      });
      [parallel endEncoding];
      continue;
    }

    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:descriptor];
    PrepareEncoder(encoder, scenario);
    EncodeRange(encoder, scenario, pass.first, pass.count, ring, ring_offset, ring_buffer);
    if (scenario.indirect && indirect_ranges_[p].second) {
      [encoder useResource:vertices_ usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
      [encoder useResource:indices_ usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
      [encoder useResource:baked_constants_ usage:MTLResourceUsageRead
                    stages:MTLRenderStageVertex | MTLRenderStageFragment];
      [encoder useResource:texture_table_ usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
      [encoder executeCommandsInBuffer:indirect_
                             withRange:NSMakeRange(indirect_ranges_[p].first, indirect_ranges_[p].second)];
    }
    [encoder endEncoding];
  }
}

void Bench::RunScenario(const Scenario& scenario, int frames, std::vector<double>& cpu, std::vector<double>& gpu) {
  dispatch_semaphore_t in_flight = dispatch_semaphore_create(kFramesInFlight);
  std::mutex gpu_mutex;
  std::mutex* gpu_mutex_pointer = &gpu_mutex;
  std::vector<double>* gpu_times = &gpu;
  double next_frame = NowMs();
  for (int frame = 0; frame < frames + 20; ++frame) {
    dispatch_semaphore_wait(in_flight, DISPATCH_TIME_FOREVER);
    @autoreleasepool {
      double start = NowMs();
      id<MTLCommandBuffer> command_buffer = [queue_ commandBuffer];
      EncodeFrame(command_buffer, scenario, frame);
      const bool measured = frame >= 20;
      [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> finished) {
        if (finished.error) fprintf(stderr, "gpu error: %s\n", finished.error.localizedDescription.UTF8String);
        if (measured) {
          std::lock_guard<std::mutex> lock(*gpu_mutex_pointer);
          gpu_times->push_back((finished.GPUEndTime - finished.GPUStartTime) * 1000.0);
        }
        dispatch_semaphore_signal(in_flight);
      }];
      [command_buffer commit];
      if (measured) cpu.push_back(NowMs() - start);
    }
    next_frame += kFrameMs;
    double wait = next_frame - NowMs();
    if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(wait));
    else next_frame = NowMs();
  }
  // Drain the GPU, then restore the count: libdispatch traps on a semaphore disposed below its start.
  for (int i = 0; i < kFramesInFlight; ++i) dispatch_semaphore_wait(in_flight, DISPATCH_TIME_FOREVER);
  for (int i = 0; i < kFramesInFlight; ++i) dispatch_semaphore_signal(in_flight);
}

// Stands in for the game: threads that run flat out touching memory, as the recompiled CPU code does.
class Load {
 public:
  explicit Load(int threads) {
    for (int i = 0; i < threads; ++i) {
      workers_.emplace_back([this] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        std::vector<uint64_t> memory(1 << 20);
        uint64_t x = 88172645463325252ull;
        while (!stop_.load(std::memory_order_relaxed)) {
          for (int i = 0; i < 4096; ++i) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            memory[x & (memory.size() - 1)] += x;
          }
        }
      });
    }
  }
  ~Load() {
    stop_ = true;
    for (std::thread& worker : workers_) worker.join();
  }

 private:
  std::atomic<bool> stop_{false};
  std::vector<std::thread> workers_;
};

}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    int frames = argc > 1 ? atoi(argv[1]) : 150;
    int rounds = argc > 2 ? atoi(argv[2]) : 3;
    // The emulator's raw draw count, with every piece of state changing on every draw.
    if (argc > 3 && !strcmp(argv[3], "stress")) {
      kDraws = 6207;
      kPipelineChange = kTextureChange = kSecondTexture = kDepthChange = kScissorChange = 1.0;
    }
    Bench bench;
    if (!bench.Setup()) return 1;

    const Scenario scenarios[] = {
        {"bound textures, 1 thread", false, 1, false},
        {"bindless, 1 thread", true, 1, false},
        {"bindless, 2 threads", true, 2, false},
        {"bindless, 3 threads", true, 3, false},
        {"bindless + baked replay", true, 1, true},
    };
    const int loads[] = {0, 2, 3};
    struct Key {
      std::string name;
      int load;
      bool operator<(const Key& other) const {
        return name != other.name ? name < other.name : load < other.load;
      }
    };
    std::map<Key, std::vector<double>> cpu, gpu;
    // Interleaved so heat lands on every case alike.
    for (int round = 0; round < rounds; ++round) {
      for (int load : loads) {
        Load busy(load);
        for (const Scenario& scenario : scenarios) {
          Key key{scenario.name, load};
          bench.RunScenario(scenario, frames, cpu[key], gpu[key]);
        }
      }
    }
    printf("%-28s %5s %8s %8s %8s %9s %8s\n", "case", "load", "cpu p50", "cpu p95", "cpu max", "us/draw", "gpu p50");
    for (const Scenario& scenario : scenarios) {
      for (int load : loads) {
        Key key{scenario.name, load};
        double p50 = Percentile(cpu[key], 0.5);
        printf("%-28s %5d %8.2f %8.2f %8.2f %9.2f %8.2f\n", scenario.name, load, p50, Percentile(cpu[key], 0.95),
               Percentile(cpu[key], 1.0), p50 * 1000.0 / kDraws, Percentile(gpu[key], 0.5));
      }
    }
  }
  return 0;
}
