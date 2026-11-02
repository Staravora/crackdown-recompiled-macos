#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
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
constexpr size_t kMaxTextures = 16384;
constexpr size_t kMaxSamplers = 64;
constexpr int kTextureHeap = 0;
constexpr int kStackHeap = 1;
constexpr int kCubeHeap = 2;
constexpr NSUInteger kGammaEntries = 256;
constexpr uint32_t kGammaMask = 0x3FF;
constexpr int kSamplerHeap = 3;
constexpr uint32_t kAlphaTestConstant = 1 << 1;

constexpr uint32_t kStencilEnable = 1 << 0;
constexpr uint32_t kDepthEnable = 1 << 1;
constexpr uint32_t kDepthWrite = 1 << 2;
constexpr uint32_t kDepthFunctionShift = 4;
constexpr uint32_t kBackFaceEnable = 1 << 7;
constexpr uint32_t kCullFront = 1 << 0;
constexpr uint32_t kCullBack = 1 << 1;
constexpr uint32_t kFrontClockwise = 1 << 2;
constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float_Stencil8;
constexpr MTLPixelFormat kFloatColorFormat = MTLPixelFormatRGBA16Float;
constexpr uint32_t kOpaqueBlend = 0x00010001;

// Grey clay: light from the slope of the depth, since there are no normals or textures to go on yet.
const char* kClaySource = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 display_vertex(uint id [[vertex_id]]) {
  float2 corner = float2((id << 1) & 2, id & 2);
  return float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
vertex float4 clear_vertex(uint id [[vertex_id]], constant float& depth [[buffer(4)]]) {
  float2 corner = float2((id << 1) & 2, id & 2);
  return float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0);
}
fragment float4 clear_fragment(constant float4& color [[buffer(4)]]) {
  return color;
}
fragment float4 display_fragment(float4 position [[position]], texture2d<float> source [[texture(0)]],
                                 texture1d<float> ramp [[texture(1)]]) {
  uint3 color = uint3(source.read(uint2(position.xy)).rgb * 255.0 + 0.5);
  return float4(ramp.read(color.r).r, ramp.read(color.g).g, ramp.read(color.b).b, 1.0);
}
// A picture into guest memory as the Xenos lays out a 32-bit texture: by 32x32 tiles, as texture_util::GetTiledOffset2D
// does, and with each pixel's four bytes reversed from Metal's order for an 8-in-32 texture.
struct GuestWrite {
  uint at;
  uint pitch;
  uint tiled;
  uint swap;
  uint2 size;   // In the guest's pixels.
  float ratio;  // The picture's pixels to one of the guest's.
};
uint tiled_offset(int x, int y, uint pitch) {
  pitch = (pitch + 31) & ~31u;
  int macro = ((x >> 5) + (y >> 5) * int(pitch >> 5)) << 9;
  int micro = ((x & 7) + ((y & 0xE) << 2)) << 2;
  int offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return uint(((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) +
              (offset & 0x3F));
}
kernel void write_guest(texture2d<float, access::read> picture [[texture(0)]], device uint* guest [[buffer(0)]],
                        constant GuestWrite& write [[buffer(1)]], uint2 at [[thread_position_in_grid]]) {
  if (at.x >= write.size.x || at.y >= write.size.y) {
    return;
  }
  const uint4 c = uint4(round(saturate(picture.read(uint2((float2(at) + 0.5) * write.ratio))) * 255.0));
  const uint value = write.swap ? (c.a | (c.r << 8) | (c.g << 16) | (c.b << 24)) : (c.b | (c.g << 8) | (c.r << 16) | (c.a << 24));
  const uint offset = write.tiled ? tiled_offset(int(at.x), int(at.y), write.pitch) : (at.y * write.pitch + at.x) * 4;
  guest[(write.at + offset) / 4] = value;
}
// A copy between textures drawn at different scales, taking the nearest pixel, as a resolve takes them one for one.
struct Resample {
  int2 from;
  int2 to;
  uint2 size;    // In the destination's pixels.
  float2 ratio;  // The source's pixels to one of the destination's.
};
kernel void resample(texture2d<float, access::read> from [[texture(0)]], texture2d<float, access::write> to [[texture(1)]],
                     constant Resample& copy [[buffer(0)]], uint2 at [[thread_position_in_grid]]) {
  if (at.x >= copy.size.x || at.y >= copy.size.y) {
    return;
  }
  to.write(from.read(uint2(float2(copy.from) + (float2(at) + 0.5) * copy.ratio)), uint2(copy.to + int2(at)));
}
kernel void resample_depth(depth2d<float, access::read> from [[texture(0)]], texture2d<float, access::write> to [[texture(1)]],
                           constant Resample& copy [[buffer(0)]], uint2 at [[thread_position_in_grid]]) {
  if (at.x >= copy.size.x || at.y >= copy.size.y) {
    return;
  }
  to.write(float4(from.read(uint2(float2(copy.from) + (float2(at) + 0.5) * copy.ratio))), uint2(copy.to + int2(at)));
}
// Where each pixel was in the last frame, if only the camera moved: through this frame's inverse view-projection to the
// world, and through the last frame's back to the screen. The game's depth is reversed and infinite, so a pixel's clip
// position is in proportion to (x, y, depth, 1), and the sky, at depth 0, comes out as a direction.
struct CameraMotion {
  float4 inverse[4];
  float4 previous[4];
  float2 size;
};
kernel void camera_motion(depth2d<float, access::read> depth [[texture(0)]], texture2d<float, access::write> motion [[texture(1)]],
                          constant CameraMotion& camera [[buffer(0)]], uint2 at [[thread_position_in_grid]]) {
  if (at.x >= depth.get_width() || at.y >= depth.get_height()) {
    return;
  }
  const float2 pixel = float2(at) + 0.5;
  const float4 clip = float4(pixel.x / camera.size.x * 2.0 - 1.0, 1.0 - pixel.y / camera.size.y * 2.0, depth.read(at), 1.0);
  const float4 world = float4(dot(camera.inverse[0], clip), dot(camera.inverse[1], clip), dot(camera.inverse[2], clip),
                              dot(camera.inverse[3], clip));
  const float4 before = float4(dot(camera.previous[0], world), dot(camera.previous[1], world), dot(camera.previous[2], world),
                               dot(camera.previous[3], world));
  float2 moved = float2(0.0);
  if (abs(before.w) > 1e-6) {
    const float2 ndc = before.xy / before.w;
    moved = float2((ndc.x + 1.0) * 0.5 * camera.size.x, (1.0 - ndc.y) * 0.5 * camera.size.y) - pixel;
  }
  motion.write(float4(clamp(moved, -2048.0, 2048.0), 0.0, 0.0), at);
}
fragment float4 clay_fragment(float4 position [[position]], constant float4& tint [[buffer(4)]]) {
  if (tint.a > 0.5) {
    return tint;
  }
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

MTLStencilOperation StencilOperationOf(uint32_t xenos) {
  constexpr std::array operations = {MTLStencilOperationKeep,           MTLStencilOperationZero,
                                     MTLStencilOperationReplace,        MTLStencilOperationIncrementClamp,
                                     MTLStencilOperationDecrementClamp, MTLStencilOperationInvert,
                                     MTLStencilOperationIncrementWrap,  MTLStencilOperationDecrementWrap};
  return operations[xenos & 7];
}

// Function, fail, depth pass and depth fail, 3 bits each, from `control`, and the masks from `reference_mask`.
MTLStencilDescriptor* StencilDescriptorOf(uint32_t control, uint32_t reference_mask) {
  MTLStencilDescriptor* stencil = [MTLStencilDescriptor new];
  stencil.stencilCompareFunction = MTLCompareFunction(control & 7);
  stencil.stencilFailureOperation = StencilOperationOf(control >> 3);
  stencil.depthStencilPassOperation = StencilOperationOf(control >> 6);
  stencil.depthFailureOperation = StencilOperationOf(control >> 9);
  stencil.readMask = (reference_mask >> 8) & 0xFF;
  stencil.writeMask = (reference_mask >> 16) & 0xFF;
  return stencil;
}

MTLBlendFactor BlendFactorOf(uint32_t xenos) {
  switch (xenos) {
    case 0:
      return MTLBlendFactorZero;
    case 1:
      return MTLBlendFactorOne;
    case 4:
      return MTLBlendFactorSourceColor;
    case 5:
      return MTLBlendFactorOneMinusSourceColor;
    case 6:
      return MTLBlendFactorSourceAlpha;
    case 7:
      return MTLBlendFactorOneMinusSourceAlpha;
    case 8:
      return MTLBlendFactorDestinationColor;
    case 9:
      return MTLBlendFactorOneMinusDestinationColor;
    case 10:
      return MTLBlendFactorDestinationAlpha;
    case 11:
      return MTLBlendFactorOneMinusDestinationAlpha;
    case 12:
      return MTLBlendFactorBlendColor;
    case 13:
      return MTLBlendFactorOneMinusBlendColor;
    case 14:
      return MTLBlendFactorBlendAlpha;
    case 15:
      return MTLBlendFactorOneMinusBlendAlpha;
    case 16:
      return MTLBlendFactorSourceAlphaSaturated;
    default:
      return MTLBlendFactorOne;
  }
}

MTLBlendOperation BlendOperationOf(uint32_t xenos) {
  constexpr std::array operations = {MTLBlendOperationAdd, MTLBlendOperationSubtract, MTLBlendOperationMin,
                                     MTLBlendOperationMax, MTLBlendOperationReverseSubtract};
  return operations[xenos < operations.size() ? xenos : 0];
}

// 6 and 7 aren't defined. Like the emulator, they're taken as 4 (zero) and 5 (one).
MTLTextureSwizzle SwizzleOf(uint32_t xenos) {
  constexpr std::array swizzles = {MTLTextureSwizzleRed,  MTLTextureSwizzleGreen, MTLTextureSwizzleBlue,
                                   MTLTextureSwizzleAlpha, MTLTextureSwizzleZero,  MTLTextureSwizzleOne};
  return swizzles[(xenos & 7) >= 4 ? (xenos & 5) : xenos];
}

MTLPixelFormat PixelFormatOf(MetalTextureFormat format) {
  switch (format) {
    case MetalTextureFormat::kRgba8:
      return MTLPixelFormatRGBA8Unorm;
    case MetalTextureFormat::kRgba8Srgb:
      return MTLPixelFormatRGBA8Unorm_sRGB;
    case MetalTextureFormat::kBc1:
      return MTLPixelFormatBC1_RGBA;
    case MetalTextureFormat::kBc1Srgb:
      return MTLPixelFormatBC1_RGBA_sRGB;
    case MetalTextureFormat::kBc2:
      return MTLPixelFormatBC2_RGBA;
    case MetalTextureFormat::kBc2Srgb:
      return MTLPixelFormatBC2_RGBA_sRGB;
    case MetalTextureFormat::kBc3:
      return MTLPixelFormatBC3_RGBA;
    case MetalTextureFormat::kBc3Srgb:
      return MTLPixelFormatBC3_RGBA_sRGB;
    case MetalTextureFormat::kBc5:
      return MTLPixelFormatBC5_RGUnorm;
    case MetalTextureFormat::kR8:
      return MTLPixelFormatR8Unorm;
    case MetalTextureFormat::kRg8:
      return MTLPixelFormatRG8Unorm;
    case MetalTextureFormat::kB5G6R5:
      return MTLPixelFormatB5G6R5Unorm;
    case MetalTextureFormat::kRgb10A2:
      return MTLPixelFormatRGB10A2Unorm;
    case MetalTextureFormat::kR16:
      return MTLPixelFormatR16Unorm;
    case MetalTextureFormat::kRg16:
      return MTLPixelFormatRG16Unorm;
    case MetalTextureFormat::kRgba16:
      return MTLPixelFormatRGBA16Unorm;
    case MetalTextureFormat::kR16Float:
      return MTLPixelFormatR16Float;
    case MetalTextureFormat::kRg16Float:
      return MTLPixelFormatRG16Float;
    case MetalTextureFormat::kRgba16Float:
      return MTLPixelFormatRGBA16Float;
    case MetalTextureFormat::kR32Float:
      return MTLPixelFormatR32Float;
    case MetalTextureFormat::kRg32Float:
      return MTLPixelFormatRG32Float;
    case MetalTextureFormat::kRgba32Float:
      return MTLPixelFormatRGBA32Float;
  }
  return MTLPixelFormatRGBA8Unorm;
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
      query_samples_[i] = [device_ newBufferWithLength:kMaxQueries * sizeof(uint64_t) options:MTLResourceStorageModeShared];
      readbacks_[i] = [device_ newBufferWithLength:kReadbackBytes options:MTLResourceStorageModeShared];
    }
    elements_ = [device_ newBufferWithLength:kElementsBytes options:untracked];

    NSError* error = nil;
    id<MTLLibrary> library = [device_ newLibraryWithSource:@(kClaySource) options:nil error:&error];
    if (!library) {
      return false;
    }
    clay_ = [library newFunctionWithName:@"clay_fragment"];
    clear_vertex_ = [library newFunctionWithName:@"clear_vertex"];
    clear_fragment_ = [library newFunctionWithName:@"clear_fragment"];
    MTLDepthStencilDescriptor* keep_depth = [MTLDepthStencilDescriptor new];
    clear_keeping_depth_ = [device_ newDepthStencilStateWithDescriptor:keep_depth];
    MTLDepthStencilDescriptor* write_depth = [MTLDepthStencilDescriptor new];
    write_depth.depthWriteEnabled = YES;
    MTLStencilDescriptor* write_stencil = [MTLStencilDescriptor new];
    write_stencil.depthStencilPassOperation = MTLStencilOperationReplace;
    write_depth.frontFaceStencil = write_stencil;
    write_depth.backFaceStencil = write_stencil;
    clear_writing_depth_ = [device_ newDepthStencilStateWithDescriptor:write_depth];
    MTLRenderPipelineDescriptor* display = [MTLRenderPipelineDescriptor new];
    display.vertexFunction = [library newFunctionWithName:@"display_vertex"];
    display.fragmentFunction = [library newFunctionWithName:@"display_fragment"];
    display.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    display_pipeline_ = [device_ newRenderPipelineStateWithDescriptor:display error:&error];
    MTLTextureDescriptor* ramp = [MTLTextureDescriptor new];
    ramp.textureType = MTLTextureType1D;
    ramp.pixelFormat = MTLPixelFormatRGBA16Unorm;
    ramp.width = kGammaEntries;
    ramp.storageMode = MTLStorageModeShared;
    gamma_ramp_ = [device_ newTextureWithDescriptor:ramp];
    write_guest_ = [device_ newComputePipelineStateWithFunction:[library newFunctionWithName:@"write_guest"] error:&error];
    motion_pipeline_ = [device_ newComputePipelineStateWithFunction:[library newFunctionWithName:@"camera_motion"] error:&error];
    resample_ = [device_ newComputePipelineStateWithFunction:[library newFunctionWithName:@"resample"] error:&error];
    resample_depth_ = [device_ newComputePipelineStateWithFunction:[library newFunctionWithName:@"resample_depth"] error:&error];
    if (!display_pipeline_ || !gamma_ramp_) {
      return false;
    }
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
      descriptor.depthAttachmentPixelFormat = kDepthFormat;
      descriptor.stencilAttachmentPixelFormat = kDepthFormat;
      id<MTLRenderPipelineState> depth_only = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&failure];
      descriptor.fragmentFunction = clay_;
      descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
      id<MTLRenderPipelineState> pipeline = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&failure];
      if (!pipeline || !depth_only) {
        error = failure.localizedDescription.UTF8String;
        return -1;
      }
      std::memcpy(static_cast<uint8_t*>(elements_.contents) + elements_used_, elements, bytes);
      pipelines_.push_back({pipeline, depth_only, elements_used_, vertex});
      elements_used_ += bytes;
      return int(pipelines_.size()) - 1;
    }
  }

  int AddPixelShader(const std::filesystem::path& path, std::string& error) override {
    @autoreleasepool {
      NSError* failure = nil;
      id<MTLLibrary> library = [device_ newLibraryWithURL:[NSURL fileURLWithPath:@(path.c_str())] error:&failure];
      if (!library) {
        error = failure.localizedDescription.UTF8String;
        return -1;
      }
      PixelShader shader;
      for (int alpha_test = 0; alpha_test < 2; ++alpha_test) {
        uint32_t constants = alpha_test ? kAlphaTestConstant : 0;
        MTLFunctionConstantValues* values = [MTLFunctionConstantValues new];
        [values setConstantValue:&constants type:MTLDataTypeUInt atIndex:0];
        shader.functions[alpha_test] = [library newFunctionWithName:@"shaderMain" constantValues:values error:&failure];
        if (!shader.functions[alpha_test]) {
          error = failure.localizedDescription.UTF8String;
          return -1;
        }
      }
      pixel_shaders_.push_back(shader);
      return int(pixel_shaders_.size()) - 1;
    }
  }

  int AddTexture(const MetalTextureDesc& desc) override {
    if (next_texture_ >= kMaxTextures) {
      return -1;
    }
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:PixelFormatOf(desc.format)
                                                                                          width:desc.width
                                                                                         height:desc.height
                                                                                      mipmapped:NO];
    if (desc.cube) {
      descriptor.textureType = MTLTextureTypeCube;
    }
    descriptor.mipmapLevelCount = desc.levels;
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.swizzle = MTLTextureSwizzleChannelsMake(SwizzleOf(desc.swizzle[0]), SwizzleOf(desc.swizzle[1]),
                                                       SwizzleOf(desc.swizzle[2]), SwizzleOf(desc.swizzle[3]));
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor];
    if (!texture) {
      return -1;
    }
    const int index = int(next_texture_++);
    textures_.resize(next_texture_);
    textures_[size_t(index)] = texture;
    static_cast<MTLResourceID*>(heaps_[desc.cube ? kCubeHeap : kTextureHeap].contents)[index] = texture.gpuResourceID;
    return index;
  }

  void FillTexture(int descriptor, uint32_t level, uint32_t face, uint32_t width, uint32_t height, const void* data,
                   size_t bytes_per_row) override {
    [textures_[size_t(descriptor)] replaceRegion:MTLRegionMake2D(0, 0, width, height)
                                     mipmapLevel:level
                                           slice:face
                                       withBytes:data
                                     bytesPerRow:bytes_per_row
                                   bytesPerImage:0];
  }

  int AddSampler(const MetalSamplerDesc& desc) override {
    if (next_sampler_ >= kMaxSamplers) {
      return 0;
    }
    constexpr std::array modes = {MTLSamplerAddressModeRepeat, MTLSamplerAddressModeMirrorRepeat,
                                  MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeMirrorClampToEdge};
    MTLSamplerDescriptor* descriptor = [MTLSamplerDescriptor new];
    descriptor.supportArgumentBuffers = YES;
    descriptor.sAddressMode = modes[size_t(desc.address_u)];
    descriptor.tAddressMode = modes[size_t(desc.address_v)];
    descriptor.minFilter = desc.nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    descriptor.magFilter = descriptor.minFilter;
    descriptor.mipFilter = desc.nearest ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    descriptor.maxAnisotropy = std::clamp<NSUInteger>(desc.max_anisotropy, 1, 16);
    id<MTLSamplerState> sampler = [device_ newSamplerStateWithDescriptor:descriptor];
    const int index = int(next_sampler_++);
    samplers_.push_back(sampler);
    static_cast<MTLResourceID*>(heaps_[kSamplerHeap].contents)[index] = sampler.gpuResourceID;
    return index;
  }

  int AddResolve(uint32_t width, uint32_t height, uint32_t levels, MetalResolveFormat format, float scale) override {
    const bool depth = format == MetalResolveFormat::kDepth;
    width = uint32_t(Scaled(int32_t(width), scale));
    height = uint32_t(Scaled(int32_t(height), scale));
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth ? MTLPixelFormatR32Float
                                                                 : format == MetalResolveFormat::kFloat ? kFloatColorFormat
                                                                                                        : MTLPixelFormatBGRA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    descriptor.mipmapLevelCount = levels;
    // Written by resample when a copy comes from a target at another scale.
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
    descriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor];
    if (!texture) {
      return -1;
    }
    if (depth) {
      ReserveDepthCopy(NSUInteger(width) * height * 4);
    }
    resolves_.push_back(texture);
    resolve_scales_.push_back(scale);
    return int(resolves_.size()) - 1;
  }

  int ViewResolve(int resolve, bool gamma, const uint8_t* swizzle) override {
    if (next_texture_ >= kMaxTextures) {
      return 0;
    }
    id<MTLTexture> storage = resolves_[size_t(resolve)];
    const MTLPixelFormat format = storage.pixelFormat != MTLPixelFormatBGRA8Unorm ? storage.pixelFormat
                                  : gamma                                         ? MTLPixelFormatRGBA8Unorm_sRGB
                                                                                  : MTLPixelFormatRGBA8Unorm;
    id<MTLTexture> view = [storage newTextureViewWithPixelFormat:format
                                                     textureType:MTLTextureType2D
                                                          levels:NSMakeRange(0, storage.mipmapLevelCount)
                                                          slices:NSMakeRange(0, 1)
                                                         swizzle:MTLTextureSwizzleChannelsMake(SwizzleOf(swizzle[0]), SwizzleOf(swizzle[1]),
                                                                                               SwizzleOf(swizzle[2]), SwizzleOf(swizzle[3]))];
    if (!view) {
      return 0;
    }
    const int index = int(next_texture_++);
    textures_.resize(next_texture_);
    textures_[size_t(index)] = view;
    static_cast<MTLResourceID*>(heaps_[kTextureHeap].contents)[index] = view.gpuResourceID;
    return index;
  }

  int AddStack(uint32_t width, uint32_t height, uint32_t depth, float scale) override {
    if (next_texture_ >= kMaxTextures) {
      return 0;
    }
    width = uint32_t(Scaled(int32_t(width), scale));
    height = uint32_t(Scaled(int32_t(height), scale));
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = MTLTextureType3D;
    descriptor.pixelFormat = MTLPixelFormatR32Float;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.depth = depth;
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor];
    if (!texture) {
      return 0;
    }
    ReserveDepthCopy(NSUInteger(width) * height * 4);
    const int index = int(next_texture_++);
    textures_.resize(next_texture_);
    textures_[size_t(index)] = texture;
    static_cast<MTLResourceID*>(heaps_[kStackHeap].contents)[index] = texture.gpuResourceID;
    stack_scales_[index] = scale;
    return index;
  }

  void CopyDepthToStack(int target, int stack, uint32_t slice) override {
    EndPass();
    const Target& source = Resolved(target);
    const auto stack_scale = stack_scales_.find(stack);
    if (stack_scale == stack_scales_.end() || source.scale != stack_scale->second) {
      dropped_stack_copies_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    id<MTLTexture> from = ReadDepth(source);
    id<MTLTexture> to = textures_[size_t(stack)];
    CopyDepth(from, MTLOriginMake(0, 0, 0), MTLSizeMake(std::min(from.width, to.width), std::min(from.height, to.height), 1), to, 0,
              MTLOriginMake(0, 0, slice));
  }

  void ClearRect(const MetalRectClear& clear) override {
    const Target& target = targets_[size_t(clear.target)];
    const int32_t width = int32_t(target.depth.width), height = int32_t(target.depth.height);
    const int32_t left = std::max(Scaled(clear.left, target.scale), 0), top = std::max(Scaled(clear.top, target.scale), 0);
    const int32_t right = std::min(Scaled(clear.right, target.scale), width), bottom = std::min(Scaled(clear.bottom, target.scale), height);
    const bool color = clear.color && target.color;
    if (right <= left || bottom <= top || (!color && !clear.depth)) {
      return;
    }
    if (left == 0 && top == 0 && right == width && bottom == height) {
      MetalClear whole;
      whole.color = color;
      whole.depth = clear.depth;
      std::copy_n(clear.color_value, 4, whole.color_value);
      whole.depth_value = clear.depth_value;
      BeginPass(clear.target, whole);
      EndPass();
      return;
    }
    BeginPass(clear.target, MetalClear{});
    [encoder_ setRenderPipelineState:ClearPipeline(target.color != nil, color, target.color.pixelFormat == kFloatColorFormat, target.samples)];
    [encoder_ setDepthStencilState:clear.depth ? clear_writing_depth_ : clear_keeping_depth_];
    [encoder_ setStencilReferenceValue:clear.stencil_value];
    [encoder_ setCullMode:MTLCullModeNone];
    [encoder_ setViewport:{0.0, 0.0, double(width), double(height), 0.0, 1.0}];
    [encoder_ setScissorRect:{NSUInteger(left), NSUInteger(top), NSUInteger(right - left), NSUInteger(bottom - top)}];
    [encoder_ setVertexBytes:&clear.depth_value length:sizeof(clear.depth_value) atIndex:4];
    [encoder_ setFragmentBytes:clear.color_value length:sizeof(clear.color_value) atIndex:4];
    [encoder_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    EndPass();
  }

  void CopyTarget(const MetalCopy& copy) override {
    EndPass();
    const Target& target = Resolved(copy.target);
    id<MTLTexture> from = copy.depth ? ReadDepth(target) : ReadColor(target);
    id<MTLTexture> to = resolves_[size_t(copy.resolve)];
    const float from_scale = target.scale, to_scale = resolve_scales_[size_t(copy.resolve)];
    const NSUInteger to_width = std::max(to.width >> copy.level, NSUInteger(1)), to_height = std::max(to.height >> copy.level, NSUInteger(1));
    // Clipped in the guest's pixels, which both sides share.
    const int32_t left = std::max(copy.left, 0), top = std::max(copy.top, 0);
    const int32_t dest_x = copy.dest_x + left - copy.left, dest_y = copy.dest_y + top - copy.top;
    const int32_t width = std::min({copy.right, int32_t(from.width / from_scale), int32_t(to_width / to_scale) - dest_x + left}) - left;
    const int32_t height = std::min({copy.bottom, int32_t(from.height / from_scale), int32_t(to_height / to_scale) - dest_y + top}) - top;
    if (width <= 0 || height <= 0 || dest_x < 0 || dest_y < 0 || (!copy.depth && from.pixelFormat != to.pixelFormat)) {
      return;
    }
    if (from_scale != to_scale) {
      Resample(from, MTLOriginMake(Scaled(left, from_scale), Scaled(top, from_scale), 0), from_scale / to_scale, to, copy.level,
               MTLOriginMake(Scaled(dest_x, to_scale), Scaled(dest_y, to_scale), 0),
               MTLSizeMake(Scaled(width, to_scale), Scaled(height, to_scale), 1), copy.depth);
      return;
    }
    const MTLOrigin from_origin = MTLOriginMake(Scaled(left, from_scale), Scaled(top, from_scale), 0);
    const MTLOrigin to_origin = MTLOriginMake(Scaled(dest_x, to_scale), Scaled(dest_y, to_scale), 0);
    const MTLSize size = MTLSizeMake(std::min({NSUInteger(Scaled(width, from_scale)), from.width - from_origin.x, to_width - to_origin.x}),
                                     std::min({NSUInteger(Scaled(height, from_scale)), from.height - from_origin.y, to_height - to_origin.y}), 1);
    if (copy.depth) {
      CopyDepth(from, from_origin, size, to, copy.level, to_origin);
      return;
    }
    id<MTLBlitCommandEncoder> blit = [buffer_ blitCommandEncoder];
    [blit copyFromTexture:from
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:from_origin
               sourceSize:size
                toTexture:to
         destinationSlice:0
         destinationLevel:copy.level
        destinationOrigin:to_origin];
    [blit endEncoding];
  }

  // A copy between a target and a resolve drawn at different scales, ratio being the source's pixels to one of the
  // destination's. Depth comes out of its depth-stencil texture as floats, as CopyDepth gives it.
  void Resample(id<MTLTexture> from, MTLOrigin from_origin, float ratio, id<MTLTexture> to, NSUInteger level, MTLOrigin to_origin,
                MTLSize size, bool depth) {
    id<MTLComputePipelineState> pipeline = depth ? resample_depth_ : resample_;
    id<MTLTexture> level_view = level ? [to newTextureViewWithPixelFormat:to.pixelFormat
                                                              textureType:MTLTextureType2D
                                                                   levels:NSMakeRange(level, 1)
                                                                   slices:NSMakeRange(0, 1)]
                                      : to;
    if (!pipeline || !level_view || !size.width || !size.height) {
      dropped_copies_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const struct {
      int32_t from[2];
      int32_t to[2];
      uint32_t size[2];
      float ratio[2];
    } resample = {{int32_t(from_origin.x), int32_t(from_origin.y)},
                  {int32_t(to_origin.x), int32_t(to_origin.y)},
                  {uint32_t(size.width), uint32_t(size.height)},
                  {ratio, ratio}};
    id<MTLComputeCommandEncoder> compute = [buffer_ computeCommandEncoder];
    [compute setComputePipelineState:pipeline];
    [compute setTexture:from atIndex:0];
    [compute setTexture:level_view atIndex:1];
    [compute setBytes:&resample length:sizeof(resample) atIndex:0];
    [compute dispatchThreads:size threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [compute endEncoding];
    resampled_copies_.fetch_add(1, std::memory_order_relaxed);
  }

  // MetalFX's spatial scaler, from the picture's size to scale times it. Null if the Mac can't.
  id<MTLTexture> Upscaled(id<MTLTexture> source, uint32_t scale) {
    const NSUInteger width = source.width * scale, height = source.height * scale;
    if (!spatial_scaler_ || spatial_scaler_.inputWidth != source.width || spatial_scaler_.inputHeight != source.height ||
        spatial_scaler_.outputWidth != width || spatial_scaler_.outputHeight != height) {
      spatial_scaler_ = nil;
      if (![MTLFXSpatialScalerDescriptor supportsDevice:device_]) {
        return nil;
      }
      MTLFXSpatialScalerDescriptor* descriptor = [MTLFXSpatialScalerDescriptor new];
      descriptor.inputWidth = source.width;
      descriptor.inputHeight = source.height;
      descriptor.outputWidth = width;
      descriptor.outputHeight = height;
      descriptor.colorTextureFormat = source.pixelFormat;
      descriptor.outputTextureFormat = source.pixelFormat;
      // The front buffer is in display terms, before the gamma ramp.
      descriptor.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
      spatial_scaler_ = [descriptor newSpatialScalerWithDevice:device_];
      MTLTextureDescriptor* output = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source.pixelFormat
                                                                                         width:width
                                                                                        height:height
                                                                                     mipmapped:NO];
      output.usage = spatial_scaler_.outputTextureUsage;
      output.storageMode = MTLStorageModePrivate;
      upscaled_ = spatial_scaler_ ? [device_ newTextureWithDescriptor:output] : nil;
    }
    if (!spatial_scaler_ || !upscaled_) {
      return nil;
    }
    spatial_scaler_.colorTexture = source;
    spatial_scaler_.outputTexture = upscaled_;
    [spatial_scaler_ encodeToCommandBuffer:buffer_];
    return upscaled_;
  }

  bool CopyPicture(int resolve, uint32_t scale) override {
    id<MTLTexture> texture = resolves_[size_t(resolve)];
    if (texture.pixelFormat != MTLPixelFormatBGRA8Unorm) {
      return false;
    }
    return QueuePicture(texture, scale);
  }

  bool QueuePicture(id<MTLTexture> texture, uint32_t scale) {
    if (picture_count_ >= kMaxPictures || (picture_count_ && (texture.width * std::max(scale, 1u) != picture_width_))) {
      return false;
    }
    if (scale > 1) {
      EndPass();
      if (id<MTLTexture> upscaled = Upscaled(texture, scale)) {
        texture = upscaled;
      }
    }
    const size_t row = texture.width * 4, bytes = row * texture.height;
    if (pictures_[picture_slot_][picture_count_].length < bytes) {
      // The swap may still be copying out of the old one.
      if (pictures_[picture_slot_][picture_count_]) {
        retired_buffers_.push_back(pictures_[picture_slot_][picture_count_]);
      }
      pictures_[picture_slot_][picture_count_] = [device_ newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    }
    id<MTLBuffer> buffer = pictures_[picture_slot_][picture_count_];
    EndPass();
    id<MTLBlitCommandEncoder> blit = [buffer_ blitCommandEncoder];
    [blit copyFromTexture:texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(texture.width, texture.height, 1)
                        toBuffer:buffer
               destinationOffset:0
          destinationBytesPerRow:row
        destinationBytesPerImage:bytes];
    [blit endEncoding];
    picture_width_ = uint32_t(texture.width);
    picture_height_ = uint32_t(texture.height);
    ++picture_count_;
    return true;
  }

  bool SnapshotScene(int target_index) override {
    const Target& target = Resolved(target_index);
    id<MTLTexture> color = ReadColor(target);
    id<MTLTexture> depth = ReadDepth(target);
    if (!color || color.pixelFormat != MTLPixelFormatBGRA8Unorm || !depth) {
      return false;
    }
    if (scene_[0].width != color.width || scene_[0].height != color.height) {
      for (auto& scene : scene_) {
        scene = NewPlainTexture(MTLPixelFormatBGRA8Unorm, color.width, color.height,
                                MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
      }
      scene_depth_ = NewPlainTexture(MTLPixelFormatDepth32Float, color.width, color.height,
                                     MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
      motion_ = NewPlainTexture(MTLPixelFormatRG16Float, color.width, color.height,
                                MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget);
      interpolated_ = NewPlainTexture(MTLPixelFormatBGRA8Unorm, color.width, color.height,
                                      MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget);
      interpolator_ = nil;
      have_scene_ = false;
      ReserveDepthCopy(color.width * color.height * 4);
    }
    EndPass();
    scene_index_ ^= 1;
    id<MTLBlitCommandEncoder> blit = [buffer_ blitCommandEncoder];
    [blit copyFromTexture:color toTexture:scene_[scene_index_]];
    [blit endEncoding];
    CopyDepth(depth, MTLOriginMake(0, 0, 0), MTLSizeMake(color.width, color.height, 1), scene_depth_, 0, MTLOriginMake(0, 0, 0));
    snapshot_this_frame_ = true;
    return true;
  }

  bool InterpolatePicture(int resolve, const MetalCamera& camera, uint32_t scale) override {
    id<MTLTexture> ui = resolves_[size_t(resolve)];
    const bool usable = snapshot_this_frame_ && have_scene_ && !camera.reset;
    have_scene_ = snapshot_this_frame_;
    snapshot_this_frame_ = false;
    if (!usable || ui.width != scene_[0].width || ui.height != scene_[0].height || !motion_pipeline_ ||
        ![MTLFXFrameInterpolatorDescriptor supportsDevice:device_]) {
      return false;
    }
    if (!interpolator_) {
      MTLFXFrameInterpolatorDescriptor* descriptor = [MTLFXFrameInterpolatorDescriptor new];
      descriptor.colorTextureFormat = MTLPixelFormatBGRA8Unorm;
      descriptor.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
      descriptor.depthTextureFormat = MTLPixelFormatDepth32Float;
      descriptor.motionTextureFormat = MTLPixelFormatRG16Float;
      descriptor.uiTextureFormat = ui.pixelFormat;
      descriptor.inputWidth = descriptor.outputWidth = ui.width;
      descriptor.inputHeight = descriptor.outputHeight = ui.height;
      interpolator_ = [descriptor newFrameInterpolatorWithDevice:device_];
      if (!interpolator_) {
        return false;
      }
    }
    EndPass();
    // In a command buffer of its own, after the frame's, so the game's fences don't wait for it. Nothing here touches guest
    // memory, and the queue keeps the next frame from overwriting what it reads.
    id<MTLCommandBuffer> frame_buffer = buffer_;
    after_ = [queue_ commandBuffer];
    buffer_ = after_;
    // How each pixel moved: back through this frame's camera to the world, and on through the last frame's.
    struct {
      std::array<float, 16> inverse;
      std::array<float, 16> previous;
      float size[2];
    } motion = {camera.inverse_view_projection, camera.previous_view_projection, {float(ui.width), float(ui.height)}};
    id<MTLComputeCommandEncoder> compute = [buffer_ computeCommandEncoder];
    [compute setComputePipelineState:motion_pipeline_];
    [compute setTexture:scene_depth_ atIndex:0];
    [compute setTexture:motion_ atIndex:1];
    [compute setBytes:&motion length:sizeof(motion) atIndex:0];
    [compute dispatchThreads:MTLSizeMake(ui.width, ui.height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [compute endEncoding];
    interpolator_.colorTexture = scene_[scene_index_];
    interpolator_.prevColorTexture = scene_[scene_index_ ^ 1];
    interpolator_.depthTexture = scene_depth_;
    interpolator_.motionTexture = motion_;
    interpolator_.uiTexture = ui;
    interpolator_.uiTextureComposited = YES;
    interpolator_.outputTexture = interpolated_;
    interpolator_.motionVectorScaleX = 1.0f;
    interpolator_.motionVectorScaleY = 1.0f;
    interpolator_.deltaTime = camera.delta_time;
    interpolator_.nearPlane = camera.near_plane;
    interpolator_.farPlane = camera.far_plane;
    interpolator_.fieldOfView = camera.field_of_view;
    interpolator_.aspectRatio = camera.aspect;
    interpolator_.depthReversed = YES;
    interpolator_.shouldResetHistory = NO;
    [interpolator_ encodeToCommandBuffer:buffer_];
    const bool queued = QueuePicture(interpolated_, scale);
    buffer_ = frame_buffer;
    return queued;
  }

  id<MTLTexture> NewPlainTexture(MTLPixelFormat format, NSUInteger width, NSUInteger height, MTLTextureUsage usage) {
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                          width:width
                                                                                         height:height
                                                                                      mipmapped:NO];
    descriptor.usage = usage;
    descriptor.storageMode = MTLStorageModePrivate;
    return [device_ newTextureWithDescriptor:descriptor];
  }

  bool WriteResolveToGuest(int resolve, size_t at, uint32_t pitch, bool tiled, bool swap) override {
    id<MTLTexture> texture = resolves_[size_t(resolve)];
    if (!guest_ || !write_guest_ || texture.pixelFormat != MTLPixelFormatBGRA8Unorm || at > UINT32_MAX) {
      return false;
    }
    EndPass();
    const float scale = resolve_scales_[size_t(resolve)];
    const uint32_t width = uint32_t(texture.width / scale), height = uint32_t(texture.height / scale);
    const struct {
      uint32_t at, pitch, tiled, swap;
      uint32_t size[2];
      float ratio;
    } write = {uint32_t(at), pitch, tiled, swap, {width, height}, scale};
    id<MTLComputeCommandEncoder> compute = [buffer_ computeCommandEncoder];
    [compute setComputePipelineState:write_guest_];
    [compute setTexture:texture atIndex:0];
    [compute setBuffer:guest_ offset:0 atIndex:0];
    [compute setBytes:&write length:sizeof(write) atIndex:1];
    [compute dispatchThreads:MTLSizeMake(width, height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [compute endEncoding];
    return true;
  }

  size_t ReadResolve(int resolve) override {
    EndPass();
    id<MTLTexture> texture = resolves_[size_t(resolve)];
    const float scale = resolve_scales_[size_t(resolve)];
    if (scale != 1.0f) {
      // Readers want the guest's pixels. write_guest, unswapped and untiled, lays them out as Metal holds them.
      const uint32_t width = uint32_t(texture.width / scale), height = uint32_t(texture.height / scale);
      const size_t bytes = size_t(width) * height * 4;
      if (texture.pixelFormat != MTLPixelFormatBGRA8Unorm || !write_guest_ || readback_used_ + bytes > kReadbackBytes) {
        dropped_reads_.fetch_add(1, std::memory_order_relaxed);
        return SIZE_MAX;
      }
      const size_t offset = readback_used_;
      readback_used_ += bytes;
      const struct {
        uint32_t at, pitch, tiled, swap;
        uint32_t size[2];
        float ratio;
      } write = {uint32_t(offset), width, 0, 0, {width, height}, scale};
      id<MTLComputeCommandEncoder> compute = [buffer_ computeCommandEncoder];
      [compute setComputePipelineState:write_guest_];
      [compute setTexture:texture atIndex:0];
      [compute setBuffer:frame_readback_ offset:0 atIndex:0];
      [compute setBytes:&write length:sizeof(write) atIndex:1];
      [compute dispatchThreads:MTLSizeMake(width, height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
      [compute endEncoding];
      return offset;
    }
    const size_t row = texture.width * 4, bytes = row * texture.height;
    if (texture.pixelFormat != MTLPixelFormatBGRA8Unorm || readback_used_ + bytes > kReadbackBytes) {
      return SIZE_MAX;
    }
    const size_t offset = readback_used_;
    readback_used_ += bytes;
    id<MTLBlitCommandEncoder> blit = [buffer_ blitCommandEncoder];
    [blit copyFromTexture:texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(texture.width, texture.height, 1)
                        toBuffer:frame_readback_
               destinationOffset:offset
          destinationBytesPerRow:row
        destinationBytesPerImage:bytes];
    [blit endEncoding];
    return offset;
  }

  MetalFrameTiming RecordFrame(const std::function<void(uint8_t* frame)>& encode, const std::filesystem::path& capture,
                               const std::function<void(const MetalFrameResults&)>& finished) override {
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
      id<MTLBuffer> query_samples = query_samples_[slot_];
      frame_query_samples_ = query_samples;
      std::memset(query_samples.contents, 0, query_samples.length);
      id<MTLBuffer> readback_pixels = readbacks_[slot_];
      frame_readback_ = readback_pixels;
      readback_used_ = 0;
      picture_slot_ = slot_;
      picture_width_ = picture_height_ = 0;
      picture_count_ = 0;
      slot_ = (slot_ + 1) % kFramesInFlight;
      frame_address_ = frame_.gpuAddress;
      shown_target_ = -1;
      encode(static_cast<uint8_t*>(frame_.contents));
      EndPass();
      buffer_ = nil;
      id<MTLCommandBuffer> after = after_;
      after_ = nil;

      id<MTLTexture> shown = shown_target_ >= 0 ? ReadColor(Resolved(shown_target_)) : nil;
      if (shown && (shown_gamma_ || shown.pixelFormat == kFloatColorFormat) && gamma_ramp_set_) {
        shown = ThroughGammaRamp(buffer, shown);
      }
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
      std::function<void(const MetalFrameResults&)> report_results = finished;
      std::array<id<MTLBuffer>, kMaxPictures> pictures{};
      for (uint32_t i = 0; i < picture_count_; ++i) {
        pictures[i] = pictures_[picture_slot_][i];
      }
      const uint32_t picture_count = picture_count_, picture_width = picture_width_, picture_height = picture_height_;
      auto picture_results = [=] {
        MetalFrameResults results;
        for (uint32_t i = 0; i < picture_count; ++i) {
          results.pictures[i] = static_cast<const uint8_t*>(pictures[i].contents);
        }
        results.picture_count = picture_count;
        results.picture_width = picture_width;
        results.picture_height = picture_height;
        return results;
      };
      // GPUStartTime counts in the same seconds.
      const CFTimeInterval committed = CACurrentMediaTime();
      [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
        NoteFinished(done);
        queued_ns_.fetch_add(uint64_t(std::max(0.0, done.GPUStartTime - committed) * 1e9), std::memory_order_relaxed);
        completed_.fetch_add(1, std::memory_order_relaxed);
        if (report_results) {
          // With frame generation, the pictures wait for its command buffer; the rest of the frame doesn't.
          MetalFrameResults results = after ? MetalFrameResults{} : picture_results();
          results.query_samples = static_cast<const uint64_t*>(query_samples.contents);
          results.readback = static_cast<const uint8_t*>(readback_pixels.contents);
          report_results(results);
        }
        dispatch_semaphore_signal(slots);
      }];
      [buffer commit];
      if (after) {
        [after addCompletedHandler:^(id<MTLCommandBuffer> done) {
          NoteFinished(done);
          generation_ns_.fetch_add(uint64_t(std::max(0.0, done.GPUEndTime - done.GPUStartTime) * 1e9), std::memory_order_relaxed);
          if (report_results) {
            MetalFrameResults results = picture_results();
            results.pictures_only = true;
            report_results(results);
          }
        }];
        [after commit];
      }
      timing.encode_ns = Since(encode_start);
      if (readback) {
        [buffer waitUntilCompleted];
        if (capture_alpha_) {
          auto* pixels = static_cast<uint8_t*>(readback.contents);
          for (size_t i = 0; i < shown.width * shown.height; ++i) {
            std::fill_n(pixels + i * 4, 3, pixels[i * 4 + 3]);
          }
        }
        WritePng(capture, readback.contents, shown.width, shown.height);
      }
    }
    return timing;
  }

  int AddTarget(uint32_t width, uint32_t height, bool has_color, bool float_color, uint32_t samples, float scale) override {
    Target target;
    target.samples = samples > 1 && [device_ supportsTextureSampleCount:samples] ? samples : 1;
    target.scale = scale;
    width = uint32_t(Scaled(int32_t(width), scale));
    height = uint32_t(Scaled(int32_t(height), scale));
    const MTLPixelFormat color_format = float_color ? kFloatColorFormat : MTLPixelFormatBGRA8Unorm;
    if (has_color) {
      target.color = NewTarget(color_format, width, height, target.samples);
    }
    target.depth = NewTarget(kDepthFormat, width, height, target.samples);
    if (target.samples > 1) {
      if (has_color) {
        target.color_resolved = NewTarget(color_format, width, height);
      }
      target.depth_resolved = NewTarget(kDepthFormat, width, height);
    }
    targets_.push_back(target);
    return int(targets_.size()) - 1;
  }

  void BeginPass(int target_index, const MetalClear& clear) override {
    EndPass();
    const Target& target = targets_[size_t(target_index)];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (target.color) {
      pass.colorAttachments[0].texture = target.color;
      pass.colorAttachments[0].loadAction = clear.color ? MTLLoadActionClear : MTLLoadActionLoad;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(clear.color_value[0], clear.color_value[1], clear.color_value[2], clear.color_value[3]);
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    pass.depthAttachment.texture = target.depth;
    pass.depthAttachment.loadAction = clear.depth ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.depthAttachment.clearDepth = clear.depth_value;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.texture = target.depth;
    pass.stencilAttachment.loadAction = clear.depth ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.stencilAttachment.clearStencil = 0;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
    pass.visibilityResultBuffer = frame_query_samples_;
    encoder_ = [buffer_ renderCommandEncoderWithDescriptor:pass];
    for (int i = 0; i < kDescriptorHeaps; ++i) {
      [encoder_ setVertexBuffer:heaps_[i] offset:0 atIndex:i];
      [encoder_ setFragmentBuffer:heaps_[i] offset:0 atIndex:i];
    }
    [encoder_ setVertexBuffer:guest_ offset:0 atIndex:kGuestBuffer];
    [encoder_ setVertexBuffer:frame_ offset:0 atIndex:kFetchBuffer];
    [encoder_ setVertexBuffer:elements_ offset:0 atIndex:kElementsBuffer];
    [encoder_ useResource:dummy_ usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
    [encoder_ useResource:dummy_3d_ usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [encoder_ useResource:dummy_cube_ usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    ++pass_number_;
    pass_has_color_ = target.color != nil;
    pass_float_ = target.color.pixelFormat == kFloatColorFormat;
    pass_samples_ = target.samples;
    pass_target_ = target_index;
    pass_scale_ = target.scale;
    pass_width_ = target.depth.width;
    pass_height_ = uint32_t(target.depth.height);
    current_scissor_ = {0, pass_height_};
    current_pipeline_ = nil;
    current_elements_ = SIZE_MAX;
    current_query_ = -1;
    current_depth_ = {};
    has_depth_state_ = false;
    current_mode_ = UINT32_MAX;
    current_viewport_ = {};
    current_bias_slope_ = 0.0f;
    std::fill_n(current_blend_constant_, 4, 0.0f);
  }

  void ShowTarget(int target, bool gamma) override {
    shown_target_ = target;
    shown_gamma_ = gamma;
  }

  void SetGammaRamp(const uint32_t* ramp) override {
    std::array<uint16_t, kGammaEntries * 4> pixels;
    for (size_t i = 0; i < kGammaEntries; ++i) {
      const uint32_t entry = ramp[i];
      const auto widen = [](uint32_t value) { return uint16_t((value & kGammaMask) * 0xFFFF / kGammaMask); };
      pixels[i * 4] = widen(entry >> 20);
      pixels[i * 4 + 1] = widen(entry >> 10);
      pixels[i * 4 + 2] = widen(entry);
      pixels[i * 4 + 3] = 0xFFFF;
    }
    [gamma_ramp_ replaceRegion:MTLRegionMake1D(0, kGammaEntries) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:0];
    gamma_ramp_set_ = true;
  }

  void CaptureAlpha(bool alpha) override { capture_alpha_ = alpha; }

  void Draw(const MetalDraw& draw) override {
    const Pipeline& pipeline = pipelines_[size_t(draw.vertex_shader)];
    id<MTLRenderPipelineState> state = pass_samples_ > 1 ? GreyPipeline(draw.vertex_shader) : pass_has_color_ ? pipeline.state : pipeline.depth_only;
    if (draw.colorless && pass_has_color_ && !pass_float_) {
      state = ColorlessPipeline(draw.vertex_shader) ?: state;
    } else if (draw.pixel_shader >= 0) {
      state = ShadedPipeline(draw) ?: state;
    } else if (pass_float_) {
      return;  // The grey pipelines are built for 8-bit colour.
    }
    if (state != current_pipeline_) {
      [encoder_ setRenderPipelineState:state];
      current_pipeline_ = state;
    }
    if (pipeline.elements != current_elements_) {
      [encoder_ setVertexBufferOffset:pipeline.elements atIndex:kElementsBuffer];
      current_elements_ = pipeline.elements;
    }
    for (uint32_t i = 0; i < draw.texture_count; ++i) {
      const size_t index = size_t(draw.textures[i]);
      if (index && index < textures_.size() && texture_pass_.size() > index && texture_pass_[index] == pass_number_) {
        continue;
      }
      if (index && index < textures_.size()) {
        texture_pass_.resize(textures_.size());
        texture_pass_[index] = pass_number_;
        [encoder_ useResource:textures_[index] usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
      }
    }
    if (!(draw.viewport == current_viewport_)) {
      const double scale = pass_scale_;
      [encoder_ setViewport:{draw.viewport.x * scale, draw.viewport.y * scale, draw.viewport.width * scale, draw.viewport.height * scale,
                             draw.viewport.near_depth, draw.viewport.far_depth}];
      current_viewport_ = draw.viewport;
    }
    const DepthKey depth_key = {draw.depth_control, draw.stencil_front & 0xFFFF00, draw.stencil_back & 0xFFFF00};
    if (!has_depth_state_ || depth_key != current_depth_) {
      [encoder_ setDepthStencilState:DepthState(depth_key)];
      current_depth_ = depth_key;
      has_depth_state_ = true;
    }
    if (draw.depth_control & kStencilEnable) {
      const uint32_t back = (draw.depth_control & kBackFaceEnable) ? draw.stencil_back : draw.stencil_front;
      [encoder_ setStencilFrontReferenceValue:draw.stencil_front & 0xFF backReferenceValue:back & 0xFF];
    }
    const auto scaled_row = [this](uint32_t row) {
      return row >= pass_height_ ? pass_height_ : std::min(uint32_t(Scaled(int32_t(row), pass_scale_)), pass_height_);
    };
    const std::pair<uint32_t, uint32_t> scissor = {scaled_row(draw.scissor_top), scaled_row(draw.scissor_bottom)};
    if (scissor != current_scissor_) {
      [encoder_ setScissorRect:{0, scissor.first, pass_width_, scissor.second - scissor.first}];
      current_scissor_ = scissor;
    }
    if (pass_has_color_ && std::memcmp(draw.blend_constant, current_blend_constant_, sizeof(current_blend_constant_)) != 0) {
      [encoder_ setBlendColorRed:draw.blend_constant[0] green:draw.blend_constant[1] blue:draw.blend_constant[2] alpha:draw.blend_constant[3]];
      std::memcpy(current_blend_constant_, draw.blend_constant, sizeof(current_blend_constant_));
    }
    if (draw.depth_bias_slope != current_bias_slope_) {
      // The slope is per pixel, and a scaled target's are smaller.
      [encoder_ setDepthBias:0.0f slopeScale:draw.depth_bias_slope * pass_scale_ clamp:0.0f];
      current_bias_slope_ = draw.depth_bias_slope;
    }
    const uint32_t mode = draw.mode_control & (kCullFront | kCullBack | kFrontClockwise);
    if (mode != current_mode_) {
      [encoder_ setCullMode:(mode & kCullFront) ? MTLCullModeFront : (mode & kCullBack) ? MTLCullModeBack : MTLCullModeNone];
      [encoder_ setFrontFacingWinding:(mode & kFrontClockwise) ? MTLWindingClockwise : MTLWindingCounterClockwise];
      current_mode_ = mode;
    }
    {
      const float tint[4] = {float((draw.debug_id >> 8) & 0xFF) / 255.0f, float(draw.debug_id & 0xFF) / 255.0f, 0.5f,
                             draw.debug_id >= 0 ? 1.0f : 0.0f};
      [encoder_ setFragmentBytes:tint length:sizeof(tint) atIndex:4];
    }
    const PushConstants push = {frame_address_ + draw.vertex_constants, frame_address_ + draw.pixel_constants,
                                frame_address_ + draw.shared_constants};
    [encoder_ setVertexBytes:&push length:sizeof(push) atIndex:kPushConstants];
    [encoder_ setFragmentBytes:&push length:sizeof(push) atIndex:kPushConstants];
    [encoder_ setVertexBufferOffset:draw.fetch_constants atIndex:kFetchBuffer];
    if (draw.query != current_query_) {
      [encoder_ setVisibilityResultMode:draw.query >= 0 ? MTLVisibilityResultModeCounting : MTLVisibilityResultModeDisabled
                                 offset:draw.query >= 0 ? size_t(draw.query) * sizeof(uint64_t) : 0];
      current_query_ = draw.query;
    }
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
    std::lock_guard lock(error_mutex_);
    return {completed_.load(std::memory_order_relaxed), errors_.load(std::memory_order_relaxed), last_error_,
            gpu_ns_.load(std::memory_order_relaxed), worst_gpu_ns_.load(std::memory_order_relaxed),
            queued_ns_.load(std::memory_order_relaxed), generation_ns_.load(std::memory_order_relaxed), resampled_copies_.load(std::memory_order_relaxed), dropped_copies_.load(std::memory_order_relaxed),
            dropped_stack_copies_.load(std::memory_order_relaxed), dropped_reads_.load(std::memory_order_relaxed)};
  }

 private:
  struct Pipeline {
    id<MTLRenderPipelineState> state;
    id<MTLRenderPipelineState> depth_only;
    size_t elements;
    id<MTLFunction> vertex;
  };

  struct PixelShader {
    std::array<id<MTLFunction>, 2> functions;
  };

  using ShadedKey = std::tuple<int, int, uint32_t, uint32_t, bool, bool, bool, uint32_t, bool>;
  using DepthKey = std::array<uint32_t, 3>;

  id<MTLRenderPipelineState> ColorlessPipeline(int vertex_shader) {
    const std::pair key = {vertex_shader, pass_samples_};
    const auto found = colorless_pipelines_.find(key);
    if (found != colorless_pipelines_.end()) {
      return found->second;
    }
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = pipelines_[size_t(vertex_shader)].vertex;
    descriptor.fragmentFunction = clay_;
    descriptor.depthAttachmentPixelFormat = kDepthFormat;
    descriptor.stencilAttachmentPixelFormat = kDepthFormat;
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
    descriptor.rasterSampleCount = pass_samples_;
    NSError* failure = nil;
    id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&failure];
    colorless_pipelines_[key] = state;
    return state;
  }

  // The grey pipelines for a multisampled pass. AddVertexShader builds the single-sample ones up front.
  id<MTLRenderPipelineState> GreyPipeline(int vertex_shader) {
    const std::tuple key = {vertex_shader, pass_samples_, pass_has_color_};
    const auto found = grey_pipelines_.find(key);
    if (found != grey_pipelines_.end()) {
      return found->second;
    }
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = pipelines_[size_t(vertex_shader)].vertex;
    descriptor.depthAttachmentPixelFormat = kDepthFormat;
    descriptor.stencilAttachmentPixelFormat = kDepthFormat;
    if (pass_has_color_) {
      descriptor.fragmentFunction = clay_;
      descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    }
    descriptor.rasterSampleCount = pass_samples_;
    NSError* failure = nil;
    id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&failure];
    grey_pipelines_[key] = state;
    return state;
  }

  id<MTLRenderPipelineState> ShadedPipeline(const MetalDraw& draw) {
    const bool alpha_to_coverage = draw.alpha_to_coverage && pass_samples_ > 1;
    const ShadedKey key = {draw.vertex_shader, draw.pixel_shader, draw.blend_control, draw.color_mask & 0xF, draw.alpha_test, pass_has_color_,
                           pass_float_, pass_samples_, alpha_to_coverage};
    const auto found = shaded_pipelines_.find(key);
    if (found != shaded_pipelines_.end()) {
      return found->second;
    }
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = pipelines_[size_t(draw.vertex_shader)].vertex;
    descriptor.fragmentFunction = pixel_shaders_[size_t(draw.pixel_shader)].functions[draw.alpha_test];
    descriptor.depthAttachmentPixelFormat = kDepthFormat;
    descriptor.stencilAttachmentPixelFormat = kDepthFormat;
    descriptor.rasterSampleCount = pass_samples_;
    descriptor.alphaToCoverageEnabled = alpha_to_coverage;
    // A shadow map has no colour, but a cut-out's pixel shader still decides which pixels cast.
    MTLRenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[0];
    if (pass_has_color_) {
      color.pixelFormat = pass_float_ ? kFloatColorFormat : MTLPixelFormatBGRA8Unorm;
      color.writeMask = ((draw.color_mask & 1) ? MTLColorWriteMaskRed : 0) | ((draw.color_mask & 2) ? MTLColorWriteMaskGreen : 0) |
                        ((draw.color_mask & 4) ? MTLColorWriteMaskBlue : 0) | ((draw.color_mask & 8) ? MTLColorWriteMaskAlpha : 0);
    }
    if (pass_has_color_ && draw.blend_control != kOpaqueBlend) {
      color.blendingEnabled = YES;
      color.sourceRGBBlendFactor = BlendFactorOf(draw.blend_control & 0x1F);
      color.rgbBlendOperation = BlendOperationOf((draw.blend_control >> 5) & 7);
      color.destinationRGBBlendFactor = BlendFactorOf((draw.blend_control >> 8) & 0x1F);
      color.sourceAlphaBlendFactor = BlendFactorOf((draw.blend_control >> 16) & 0x1F);
      color.alphaBlendOperation = BlendOperationOf((draw.blend_control >> 21) & 7);
      color.destinationAlphaBlendFactor = BlendFactorOf((draw.blend_control >> 24) & 0x1F);
    }
    NSError* failure = nil;
    id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&failure];
    if (!state) {
      ++pipeline_failures_;
      last_pipeline_error_ = failure.localizedDescription.UTF8String;
    }
    shaded_pipelines_[key] = state;
    return state;
  }

  // A multisampled target draws into color and depth, and keeps a single-sample copy of each for anything that reads it,
  // brought up to date only when something does.
  struct Target {
    id<MTLTexture> color;
    id<MTLTexture> depth;
    id<MTLTexture> color_resolved;
    id<MTLTexture> depth_resolved;
    uint32_t samples = 1;
    float scale = 1.0f;  // Its pixels to one of the guest's, each way.
    bool unresolved = false;
  };

  // A length in the guest's pixels, in a texture's at scale.
  static int32_t Scaled(int32_t value, float scale) {
    return int32_t(std::clamp(std::round(double(value) * scale), double(INT32_MIN), double(INT32_MAX)));
  }

  id<MTLTexture> NewTarget(MTLPixelFormat format, uint32_t width, uint32_t height, uint32_t samples = 1) {
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                          width:width
                                                                                         height:height
                                                                                      mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModePrivate;
    if (samples > 1) {
      descriptor.textureType = MTLTextureType2DMultisample;
      descriptor.sampleCount = samples;
      descriptor.usage = MTLTextureUsageRenderTarget;
    }
    return [device_ newTextureWithDescriptor:descriptor];
  }

  // Counts a finished command buffer's GPU time and any error.
  void NoteFinished(id<MTLCommandBuffer> done) {
    if (done.status == MTLCommandBufferStatusError) {
      errors_.fetch_add(1, std::memory_order_relaxed);
      std::lock_guard lock(error_mutex_);
      last_error_ = done.error.description.UTF8String;
    }
    const uint64_t gpu_ns = uint64_t(std::max(0.0, done.GPUEndTime - done.GPUStartTime) * 1e9);
    gpu_ns_.fetch_add(gpu_ns, std::memory_order_relaxed);
    uint64_t worst = worst_gpu_ns_.load(std::memory_order_relaxed);
    while (worst < gpu_ns && !worst_gpu_ns_.compare_exchange_weak(worst, gpu_ns, std::memory_order_relaxed)) {
    }
  }

  void EndPass() {
    if (encoder_ && pass_target_ >= 0 && targets_[size_t(pass_target_)].samples > 1) {
      targets_[size_t(pass_target_)].unresolved = true;
    }
    [encoder_ endEncoding];
    encoder_ = nil;
    pass_target_ = -1;
  }

  // Brings a multisampled target's single-sample copies up to date: a pass that loads it, draws nothing and resolves.
  // Colour averages its samples and depth takes the first, as the Xenos's resolve does.
  const Target& Resolved(int index) {
    Target& target = targets_[size_t(index)];
    if (target.samples > 1 && target.unresolved) {
      EndPass();
      MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      if (target.color) {
        pass.colorAttachments[0].texture = target.color;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
        pass.colorAttachments[0].resolveTexture = target.color_resolved;
      }
      pass.depthAttachment.texture = target.depth;
      pass.depthAttachment.loadAction = MTLLoadActionLoad;
      pass.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
      pass.depthAttachment.resolveTexture = target.depth_resolved;
      pass.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
      [[buffer_ renderCommandEncoderWithDescriptor:pass] endEncoding];
      target.unresolved = false;
    }
    return target;
  }

  static id<MTLTexture> ReadColor(const Target& target) { return target.samples > 1 ? target.color_resolved : target.color; }
  static id<MTLTexture> ReadDepth(const Target& target) { return target.samples > 1 ? target.depth_resolved : target.depth; }

  id<MTLTexture> ThroughGammaRamp(id<MTLCommandBuffer> buffer, id<MTLTexture> source) {
    if (display_.width != source.width || display_.height != source.height) {
      display_ = NewTarget(MTLPixelFormatBGRA8Unorm, uint32_t(source.width), uint32_t(source.height));
    }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = display_;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:display_pipeline_];
    [encoder setFragmentTexture:source atIndex:0];
    [encoder setFragmentTexture:gamma_ramp_ atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    return display_;
  }

  id<MTLRenderPipelineState> ClearPipeline(bool has_color, bool write_color, bool float_color, uint32_t samples) {
    const size_t index = (has_color ? (write_color ? 2 : 1) + (float_color ? 2 : 0) : 0) + (samples > 1 ? kClearPipelineKinds : 0);
    if (!clear_pipelines_[index]) {
      MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
      descriptor.vertexFunction = clear_vertex_;
      descriptor.depthAttachmentPixelFormat = kDepthFormat;
      descriptor.stencilAttachmentPixelFormat = kDepthFormat;
      if (has_color) {
        descriptor.fragmentFunction = clear_fragment_;
        descriptor.colorAttachments[0].pixelFormat = float_color ? kFloatColorFormat : MTLPixelFormatBGRA8Unorm;
        descriptor.colorAttachments[0].writeMask = write_color ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
      }
      descriptor.rasterSampleCount = samples;
      clear_pipelines_[index] = [device_ newRenderPipelineStateWithDescriptor:descriptor error:nil];
    }
    return clear_pipelines_[index];
  }

  void ReserveDepthCopy(NSUInteger bytes) {
    if (depth_copy_.length < bytes) {
      // A frame in flight may still be using it, and frames don't keep what they use alive.
      if (depth_copy_) {
        retired_buffers_.push_back(depth_copy_);
      }
      depth_copy_ = [device_ newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
    }
  }

  // Shaders read depth as a float texture. A depth plane only comes apart from stencil by way of a buffer.
  void CopyDepth(id<MTLTexture> from, MTLOrigin origin, MTLSize size, id<MTLTexture> to, NSUInteger level, MTLOrigin to_origin) {
    const NSUInteger row = size.width * 4;
    id<MTLBlitCommandEncoder> blit = [buffer_ blitCommandEncoder];
    [blit copyFromTexture:from
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:origin
                      sourceSize:size
                        toBuffer:depth_copy_
               destinationOffset:0
          destinationBytesPerRow:row
        destinationBytesPerImage:row * size.height
                         options:MTLBlitOptionDepthFromDepthStencil];
    [blit copyFromBuffer:depth_copy_
               sourceOffset:0
          sourceBytesPerRow:row
        sourceBytesPerImage:row * size.height
                 sourceSize:size
                  toTexture:to
           destinationSlice:0
           destinationLevel:level
          destinationOrigin:to_origin];
    [blit endEncoding];
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
      const size_t slot_count = i == kSamplerHeap ? kMaxSamplers : kMaxTextures;
      heaps_[i] = [device_ newBufferWithLength:sizeof(MTLResourceID) * slot_count options:MTLResourceStorageModeShared];
      auto* slots = static_cast<MTLResourceID*>(heaps_[i].contents);
      std::fill_n(slots, slot_count, ids[size_t(i)]);
    }
    return true;
  }

  id<MTLDepthStencilState> DepthState(const DepthKey& key) {
    const auto found = depth_states_.find(key);
    if (found != depth_states_.end()) {
      return found->second;
    }
    const uint32_t control = key[0];
    MTLDepthStencilDescriptor* descriptor = [MTLDepthStencilDescriptor new];
    descriptor.depthCompareFunction =
        (control & kDepthEnable) ? CompareFunction(control >> kDepthFunctionShift) : MTLCompareFunctionAlways;
    descriptor.depthWriteEnabled = (control & kDepthEnable) && (control & kDepthWrite);
    if (control & kStencilEnable) {
      descriptor.frontFaceStencil = StencilDescriptorOf(control >> 8, key[1]);
      descriptor.backFaceStencil =
          (control & kBackFaceEnable) ? StencilDescriptorOf(control >> 20, key[2]) : StencilDescriptorOf(control >> 8, key[1]);
    }
    id<MTLDepthStencilState> state = [device_ newDepthStencilStateWithDescriptor:descriptor];
    depth_states_[key] = state;
    return state;
  }

  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLBuffer> guest_ = nil;
  id<MTLBuffer> elements_ = nil;
  id<MTLBuffer> depth_copy_ = nil;
  std::vector<id<MTLBuffer>> retired_buffers_;
  id<MTLBuffer> frame_ = nil;
  id<MTLTexture> dummy_ = nil;
  id<MTLTexture> dummy_3d_ = nil;
  id<MTLTexture> dummy_cube_ = nil;
  id<MTLSamplerState> sampler_ = nil;
  id<MTLFunction> clay_ = nil;
  id<MTLRenderPipelineState> display_pipeline_ = nil;
  id<MTLFunction> clear_vertex_ = nil;
  id<MTLFunction> clear_fragment_ = nil;
  id<MTLDepthStencilState> clear_keeping_depth_ = nil;
  id<MTLDepthStencilState> clear_writing_depth_ = nil;
  static constexpr size_t kClearPipelineKinds = 5;
  std::array<id<MTLRenderPipelineState>, kClearPipelineKinds * 2> clear_pipelines_{};
  id<MTLTexture> gamma_ramp_ = nil;
  id<MTLTexture> display_ = nil;
  id<MTLCommandBuffer> buffer_ = nil;
  id<MTLCommandBuffer> after_ = nil;  // Frame generation's work, committed after the frame's.
  id<MTLRenderCommandEncoder> encoder_ = nil;
  CAMetalLayer* layer_ = nil;
  NSWindow* window_ = nil;
  mutable std::mutex error_mutex_;
  std::string last_error_;
  std::array<id<MTLBuffer>, kFramesInFlight> frames_{};
  std::array<id<MTLBuffer>, kFramesInFlight> query_samples_{};
  id<MTLBuffer> frame_query_samples_ = nil;
  id<MTLComputePipelineState> write_guest_ = nil;
  id<MTLComputePipelineState> resample_ = nil;
  id<MTLComputePipelineState> resample_depth_ = nil;
  std::vector<float> resolve_scales_;
  std::unordered_map<int, float> stack_scales_;  // By descriptor.
  float pass_scale_ = 1.0f;
  std::atomic<uint64_t> queued_ns_{0};
  std::atomic<uint64_t> generation_ns_{0};
  std::atomic<uint64_t> resampled_copies_{0};
  std::atomic<uint64_t> dropped_copies_{0};
  std::atomic<uint64_t> dropped_stack_copies_{0};
  std::atomic<uint64_t> dropped_reads_{0};
  static constexpr uint32_t kMaxPictures = 2;
  std::array<std::array<id<MTLBuffer>, kMaxPictures>, kFramesInFlight> pictures_{};
  uint32_t picture_count_ = 0;
  // Frame generation.
  std::array<id<MTLTexture>, 2> scene_{};
  uint32_t scene_index_ = 0;
  id<MTLTexture> scene_depth_ = nil;
  id<MTLTexture> motion_ = nil;
  id<MTLTexture> interpolated_ = nil;
  id<MTLFXFrameInterpolator> interpolator_ = nil;
  id<MTLComputePipelineState> motion_pipeline_ = nil;
  bool snapshot_this_frame_ = false;
  bool have_scene_ = false;
  id<MTLFXSpatialScaler> spatial_scaler_ = nil;
  id<MTLTexture> upscaled_ = nil;
  uint32_t picture_slot_ = 0;
  uint32_t picture_width_ = 0;
  uint32_t picture_height_ = 0;
  std::array<id<MTLBuffer>, kFramesInFlight> readbacks_{};
  id<MTLBuffer> frame_readback_ = nil;
  size_t readback_used_ = 0;
  std::array<id<MTLBuffer>, kDescriptorHeaps> heaps_{};
  std::vector<Pipeline> pipelines_;
  std::vector<PixelShader> pixel_shaders_;
  std::map<ShadedKey, id<MTLRenderPipelineState>> shaded_pipelines_;
  std::map<std::pair<int, uint32_t>, id<MTLRenderPipelineState>> colorless_pipelines_;
  std::map<std::tuple<int, uint32_t, bool>, id<MTLRenderPipelineState>> grey_pipelines_;
  std::vector<id<MTLTexture>> textures_;
  std::vector<id<MTLTexture>> resolves_;
  std::vector<id<MTLSamplerState>> samplers_;
  std::vector<uint64_t> texture_pass_;
  size_t next_texture_ = 1;
  size_t next_sampler_ = 1;
  uint64_t pass_number_ = 0;
  uint64_t pipeline_failures_ = 0;
  std::string last_pipeline_error_;
  std::vector<Target> targets_;
  std::map<DepthKey, id<MTLDepthStencilState>> depth_states_;
  dispatch_semaphore_t slots_ = nil;
  uint8_t* guest_memory_ = nullptr;
  uint64_t frame_address_ = 0;
  size_t elements_used_ = 0;
  uint32_t slot_ = 0;
  id<MTLRenderPipelineState> current_pipeline_ = nil;
  size_t current_elements_ = SIZE_MAX;
  int current_query_ = -1;
  int shown_target_ = -1;
  bool capture_alpha_ = false;
  bool shown_gamma_ = false;
  bool gamma_ramp_set_ = false;
  bool pass_has_color_ = false;
  bool pass_float_ = false;
  uint32_t pass_samples_ = 1;
  int pass_target_ = -1;
  NSUInteger pass_width_ = 0;
  uint32_t pass_height_ = 0;
  std::pair<uint32_t, uint32_t> current_scissor_{};
  MetalViewport current_viewport_;
  DepthKey current_depth_{};
  bool has_depth_state_ = false;
  uint32_t current_mode_ = UINT32_MAX;
  float current_bias_slope_ = 0.0f;
  float current_blend_constant_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
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
