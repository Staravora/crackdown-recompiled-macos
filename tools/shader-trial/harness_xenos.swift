// Times a XenosRecomp pixel shader the way experiments/menu-bench/harness times the emulator's: a
// full-screen quad at 1280x720 with 2x MSAA, dummy noise textures and random float constants, best of
// six command buffers. Usage: harness_xenos <shader.metal> [draws]
import Foundation
import Metal

let args = CommandLine.arguments
guard args.count >= 2 else { print("usage: harness_xenos file.metal [draws]"); exit(2) }
let path = args[1]
let draws = args.count > 2 ? Int(args[2])! : 20
var src = try! String(contentsOfFile: path, encoding: .utf8)
let device = MTLCreateSystemDefaultDevice()!

func matches(_ pattern: String, _ text: String) -> [[String]] {
  let re = try! NSRegularExpression(pattern: pattern, options: [.dotMatchesLineSeparators])
  let ns = text as NSString
  return re.matches(in: text, range: NSRange(location: 0, length: ns.length)).map { m in
    (0..<m.numberOfRanges).map { m.range(at: $0).location == NSNotFound ? "" : ns.substring(with: m.range(at: $0)) }
  }
}

// A vertex shader feeding every interpolator, as the emulator harness does.
var fields = ""
var assigns = ""
for (i, f) in matches("float4 \\w+ \\[\\[user\\((\\w+)\\)\\]\\]", src).enumerated() where !fields.contains("user(\(f[1]))") {
  fields += "    float4 v\(i) [[user(\(f[1]))]];\n"
  assigns += "    o.v\(i) = float4(uv, 0.5, 1.0);\n"
}
src += """

struct HarnessVSOut {
    float4 harness_position [[position]];
\(fields)};
vertex HarnessVSOut harness_vs(uint vid [[vertex_id]]) {
    float2 uv = float2(float(vid & 1u), float(vid >> 1u));
    HarnessVSOut o;
    o.harness_position = float4(uv * 2.0 - 1.0, 0.5, 1.0);
\(assigns)    return o;
}
"""

let options = MTLCompileOptions()
options.mathMode = .fast
options.preprocessorMacros = ["__air__": NSNumber(value: 1)]
let library: MTLLibrary
do { library = try device.makeLibrary(source: src, options: options) } catch {
  print("COMPILE ERROR \(path): \(error)"); exit(1)
}
let width = 1280, height = 720, samples = 2
let pd = MTLRenderPipelineDescriptor()
pd.vertexFunction = library.makeFunction(name: "harness_vs")!
do {
  pd.fragmentFunction = try library.makeFunction(name: "shaderMain", constantValues: MTLFunctionConstantValues())
} catch { print("FUNCTION ERROR \(path): \(error)"); exit(1) }
pd.rasterSampleCount = samples
let colorOutputs = matches("\\[\\[color\\((\\d+)\\)\\]\\]", src).map { Int($0[1])! }
let colorCount = max(1, (colorOutputs.max() ?? 0) + 1)
for i in 0..<colorCount { pd.colorAttachments[i].pixelFormat = .rgba8Unorm }
pd.depthAttachmentPixelFormat = .depth32Float
let pipeline: MTLRenderPipelineState
do { pipeline = try device.makeRenderPipelineState(descriptor: pd) } catch {
  print("PIPELINE ERROR \(path): \(error)"); exit(1)
}

func noiseTexture(_ type: MTLTextureType) -> MTLTexture {
  let d = MTLTextureDescriptor()
  d.textureType = type
  d.pixelFormat = .rgba8Unorm
  d.width = type == .typeCube ? 256 : (type == .type3D ? 64 : width)
  d.height = type == .typeCube ? 256 : (type == .type3D ? 64 : height)
  d.depth = type == .type3D ? 64 : 1
  d.usage = .shaderRead
  d.storageMode = .shared
  let t = device.makeTexture(descriptor: d)!
  var bytes = [UInt8](repeating: 0, count: d.width * d.height * 4)
  for i in 0..<bytes.count { bytes[i] = UInt8.random(in: 0...255) }
  if type == .type3D {
    for z in 0..<d.depth {
      t.replace(region: MTLRegionMake3D(0, 0, z, d.width, d.height, 1), mipmapLevel: 0, slice: 0,
                withBytes: bytes, bytesPerRow: d.width * 4, bytesPerImage: d.width * d.height * 4)
    }
  } else {
    for s in 0..<(type == .typeCube ? 6 : 1) {
      t.replace(region: MTLRegionMake2D(0, 0, d.width, d.height), mipmapLevel: 0, slice: s,
                withBytes: bytes, bytesPerRow: d.width * 4, bytesPerImage: 0)
    }
  }
  return t
}

let sd = MTLSamplerDescriptor()
sd.minFilter = .linear; sd.magFilter = .linear; sd.mipFilter = .notMipmapped
sd.sAddressMode = .clampToEdge; sd.tAddressMode = .clampToEdge
sd.supportArgumentBuffers = true
let sampler = device.makeSamplerState(descriptor: sd)!

// The generated code indexes tables of resource IDs; every slot here holds the same dummy.
func table(_ id: MTLResourceID) -> MTLBuffer {
  let b = device.makeBuffer(length: 16 * 1024, options: .storageModeShared)!
  let slots = b.contents().bindMemory(to: MTLResourceID.self, capacity: 2048)
  for i in 0..<2048 { slots[i] = id }
  return b
}
let texture2D = noiseTexture(.type2D), texture3D = noiseTexture(.type3D), textureCube = noiseTexture(.typeCube)
let tables = [table(texture2D.gpuResourceID), table(texture3D.gpuResourceID), table(textureCube.gpuResourceID),
              table(sampler.gpuResourceID)]

let constants = device.makeBuffer(length: 1 << 16, options: .storageModeShared)!
let floats = constants.contents().bindMemory(to: Float.self, capacity: 1 << 14)
for i in 0..<4096 { floats[i] = Float.random(in: 0.1...1.0) }
let shared = device.makeBuffer(length: 1 << 16, options: .storageModeShared)!
var push: [UInt64] = [constants.gpuAddress, constants.gpuAddress, shared.gpuAddress]

let ctd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: width, height: height, mipmapped: false)
ctd.textureType = .type2DMultisample; ctd.sampleCount = samples; ctd.usage = .renderTarget; ctd.storageMode = .private
let colors = (0..<colorCount).map { _ in device.makeTexture(descriptor: ctd)! }
let dtd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .depth32Float, width: width, height: height, mipmapped: false)
dtd.textureType = .type2DMultisample; dtd.sampleCount = samples; dtd.usage = .renderTarget; dtd.storageMode = .private
let depth = device.makeTexture(descriptor: dtd)!
let dsd = MTLDepthStencilDescriptor(); dsd.depthCompareFunction = .always; dsd.isDepthWriteEnabled = false
let dss = device.makeDepthStencilState(descriptor: dsd)!

let queue = device.makeCommandQueue()!
var best = Double.infinity
for _ in 0..<6 {
  let rp = MTLRenderPassDescriptor()
  for i in 0..<colorCount {
    rp.colorAttachments[i].texture = colors[i]; rp.colorAttachments[i].loadAction = .clear
    rp.colorAttachments[i].storeAction = .dontCare
  }
  rp.depthAttachment.texture = depth; rp.depthAttachment.loadAction = .clear; rp.depthAttachment.storeAction = .dontCare
  let cb = queue.makeCommandBuffer()!
  let e = cb.makeRenderCommandEncoder(descriptor: rp)!
  e.setRenderPipelineState(pipeline)
  e.setDepthStencilState(dss)
  for (i, b) in tables.enumerated() { e.setFragmentBuffer(b, offset: 0, index: i) }
  e.setFragmentBytes(&push, length: 24, index: 8)
  for t in [texture2D, texture3D, textureCube] as [MTLResource] { e.useResource(t, usage: .read, stages: .fragment) }
  e.useResource(constants, usage: .read, stages: .fragment)
  e.useResource(shared, usage: .read, stages: .fragment)
  for _ in 0..<draws { e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4) }
  e.endEncoding()
  cb.commit(); cb.waitUntilCompleted()
  if let err = cb.error { print("GPU ERROR \(path): \(err)"); exit(1) }
  best = min(best, (cb.gpuEndTime - cb.gpuStartTime) * 1000)
}
print(String(format: "%8.3f ms/draw  %@", best / Double(draws), (path as NSString).lastPathComponent))
