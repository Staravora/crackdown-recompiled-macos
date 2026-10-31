// Draws one pixel shader in both forms, the emulator's Metal and XenosRecomp's, from identical inputs,
// and reports how far apart the pictures are. Bool constants cycle through off, on and mixed so
// conditional blocks run. With TIME=1 it also times both at 1280x720 with 2x MSAA, one draw per pass.
// Usage: pixel_diff <job.json>
import Foundation
import Metal

struct Job: Decodable {
  let emulator: String
  let xenos: String
  let floatRegisters: [Int]       // Emulator packed index to guest register.
  let literals: [String: [UInt32]]
  let interpolators: [String: Int]  // XenosRecomp semantic to guest register.
  let loopLiterals: [String: [Int]]
  let loopIds: [Int]
}

let job = try! JSONDecoder().decode(Job.self, from: Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1])))
let device = MTLCreateSystemDefaultDevice()!
let queue = device.makeCommandQueue()!
let diffWidth = 240, diffHeight = 135
// NOISE=1 swaps in the older harness's large noise textures, to see how far timing depends on them.
let noise = ProcessInfo.processInfo.environment["NOISE"] == "1"
let noMips = ProcessInfo.processInfo.environment["NOMIPS"] == "1"
let textureSize = noMips ? 1280 : (noise ? 1024 : 256)
let timedBools = UInt32(ProcessInfo.processInfo.environment["BOOLS"] ?? "65535")!

func matches(_ pattern: String, _ text: String) -> [[String]] {
  let re = try! NSRegularExpression(pattern: pattern, options: [.dotMatchesLineSeparators])
  let ns = text as NSString
  return re.matches(in: text, range: NSRange(location: 0, length: ns.length)).map { m in
    (0..<m.numberOfRanges).map { m.range(at: $0).location == NSNotFound ? "" : ns.substring(with: m.range(at: $0)) }
  }
}

// Repeatable numbers, so both forms see the same inputs.
func unit(_ seed: Int) -> Float {
  var x = UInt32(truncatingIfNeeded: seed &* 31 &+ 7) &* 2654435761
  x ^= x >> 15
  x = x &* 2246822519
  x ^= x >> 13
  return Float(x % 10000) / 10000
}
func constantValue(_ register: Int, _ component: Int) -> Float { 0.05 + 0.95 * unit(register * 4 + component) }

func interpolatorExpression(_ register: Int) -> String {
  let c = (0..<8).map { 0.2 + unit(1000 + register * 8 + $0) }
  return "float4(uv.x * \(c[0]) + \(c[1] - 0.2), uv.y * \(c[2]) + \(c[3] - 0.2), (1.0 - uv.x) * \(c[4]) + \(c[5] - 0.2), uv.y * \(c[6]) + \(c[7] - 0.2))"
}

func vertexShader(_ fields: [(attribute: String, value: String)]) -> String {
  var members = "", assigns = ""
  for (i, f) in fields.enumerated() {
    members += "    float4 v\(i) \(f.attribute);\n"
    assigns += "    o.v\(i) = \(f.value);\n"
  }
  return """

  struct HarnessVSOut {
      float4 harness_position [[position]];
  \(members)};
  vertex HarnessVSOut harness_vs(uint vid [[vertex_id]]) {
      float2 uv = float2(float(vid & 1u), float(vid >> 1u));
      HarnessVSOut o;
      o.harness_position = float4(uv * 2.0 - 1.0, 0.5, 1.0);
  \(assigns)    return o;
  }
  """
}

// Smooth content with a full mip chain, so a wrong level or a wrong coordinate shows up.
func patternTexture(_ type: MTLTextureType) -> MTLTexture {
  let d = MTLTextureDescriptor()
  d.textureType = type
  d.pixelFormat = .rgba8Unorm
  d.width = type == .type3D ? 32 : textureSize
  d.height = d.width
  d.depth = type == .type3D ? 32 : 1
  d.arrayLength = 1
  d.mipmapLevelCount = noMips ? 1 : 1 + Int(log2(Double(d.width)))
  d.usage = .shaderRead
  d.storageMode = .shared
  let t = device.makeTexture(descriptor: d)!
  var bytes = [UInt8](repeating: 255, count: d.width * d.height * 4)
  for y in 0..<d.height {
    for x in 0..<d.width {
      let u = Float(x) / Float(d.width), v = Float(y) / Float(d.height)
      let i = (y * d.width + x) * 4
      bytes[i] = UInt8(127.5 + 127.5 * sin(u * 25 + v * 7))
      bytes[i + 1] = UInt8(127.5 + 127.5 * sin(v * 31 - u * 3))
      bytes[i + 2] = UInt8(255 * (u * 0.5 + v * 0.5))
      bytes[i + 3] = UInt8(127.5 + 127.5 * cos((u + v) * 17))
    }
  }
  if noise {
    var state: UInt64 = 88172645463325252
    for i in 0..<bytes.count {
      state ^= state << 13; state ^= state >> 7; state ^= state << 17
      bytes[i] = UInt8(truncatingIfNeeded: state >> 24)
    }
  }
  if type == .type3D {
    for z in 0..<d.depth {
      t.replace(region: MTLRegionMake3D(0, 0, z, d.width, d.height, 1), mipmapLevel: 0, slice: 0, withBytes: bytes,
                bytesPerRow: d.width * 4, bytesPerImage: d.width * d.height * 4)
    }
  } else {
    for s in 0..<(type == .typeCube ? 6 : 1) {
      t.replace(region: MTLRegionMake2D(0, 0, d.width, d.height), mipmapLevel: 0, slice: s, withBytes: bytes,
                bytesPerRow: d.width * 4, bytesPerImage: 0)
    }
  }
  if noMips { return t }
  let cb = queue.makeCommandBuffer()!
  let blit = cb.makeBlitCommandEncoder()!
  blit.generateMipmaps(for: t)
  blit.endEncoding()
  cb.commit()
  cb.waitUntilCompleted()
  return t
}
let texture2D = patternTexture(.type2D), textureArray = patternTexture(.type2DArray)
let texture3D = patternTexture(.type3D), textureCube = patternTexture(.typeCube)

let sd = MTLSamplerDescriptor()
sd.minFilter = .linear; sd.magFilter = .linear; sd.mipFilter = noMips ? .notMipmapped : .linear
sd.sAddressMode = .clampToEdge; sd.tAddressMode = .clampToEdge; sd.rAddressMode = .clampToEdge
sd.supportArgumentBuffers = true
let sampler = device.makeSamplerState(descriptor: sd)!

func newBuffer(_ length: Int) -> MTLBuffer { device.makeBuffer(length: length, options: .storageModeShared)! }

struct Form {
  let bind: (MTLRenderCommandEncoder, UInt32) -> Void
  let pipeline: (Int, MTLPixelFormat) -> MTLRenderPipelineState
}

func compile(_ source: String, macros: [String: NSObject]) -> MTLLibrary {
  let options = MTLCompileOptions()
  options.mathMode = .fast
  options.preprocessorMacros = macros
  do { return try device.makeLibrary(source: source, options: options) } catch {
    print("{\"error\":\"compile\"}"); exit(1)
  }
}

func makePipeline(_ library: MTLLibrary, _ fragment: MTLFunction, _ source: String, _ samples: Int, _ format: MTLPixelFormat) -> MTLRenderPipelineState {
  let pd = MTLRenderPipelineDescriptor()
  pd.vertexFunction = library.makeFunction(name: "harness_vs")!
  pd.fragmentFunction = fragment
  pd.rasterSampleCount = samples
  let outputs = matches("\\[\\[color\\((\\d+)\\)\\]\\]", source).map { Int($0[1])! }
  for i in 0...(outputs.max() ?? 0) { pd.colorAttachments[i].pixelFormat = format }
  pd.depthAttachmentPixelFormat = .depth32Float
  do { return try device.makeRenderPipelineState(descriptor: pd) } catch {
    print("{\"error\":\"pipeline\"}"); exit(1)
  }
}

func emulatorForm() -> Form {
  var src = try! String(contentsOfFile: job.emulator, encoding: .utf8)
  // The final multiply by the render target's exponent bias, left out as XenosRecomp has none.
  src = src.replacingOccurrences(of: "(*spvDescriptorSet1.xe_uniform_system_constants).color_exp_bias", with: "float4(1.0)")
  // Lets a run ask whether a difference comes only from how the two forms choose their mip level.
  if ProcessInfo.processInfo.environment["IMPLICIT"] == "1" {
    src = src.replacingOccurrences(of: ", gradient2d\\([^()]*\\)", with: "", options: .regularExpression)
    src = src.replacingOccurrences(of: ", gradient3d\\([^()]*\\)", with: "", options: .regularExpression)
    src = src.replacingOccurrences(of: ", spvGradientCube\\([^()]*\\)", with: "", options: .regularExpression)
  }
  var fields: [(attribute: String, value: String)] = []
  if let body = matches("struct main0_in\\s*\\{(.*?)\\};", src).first?[1] {
    for f in matches("float4\\s+xe_in_interpolator_(\\d+)\\s*(\\[\\[[^;]*\\]\\])\\s*;", body) {
      fields.append((f[2], interpolatorExpression(Int(f[1])!)))
    }
  }
  src += vertexShader(fields)
  let library = compile(src, macros: [:])
  let fs = library.makeFunction(name: "main0")!

  let floats = newBuffer(1 << 14)
  let words = floats.contents().bindMemory(to: UInt32.self, capacity: 1 << 12)
  for (i, register) in job.floatRegisters.enumerated() {
    for c in 0..<4 {
      if let literal = job.literals[String(register)] { words[i * 4 + c] = literal[c] }
      else { words[i * 4 + c] = constantValue(register, c).bitPattern }
    }
  }
  let system = newBuffer(1 << 14)
  // The emulator does alpha test in the shader, and these flag bits off mean never pass.
  system.contents().bindMemory(to: UInt32.self, capacity: 1).pointee = 7 << 16
  let boolLoop = newBuffer(1 << 12)
  let fetch = newBuffer(1 << 12)
  let fetchWords = fetch.contents().bindMemory(to: UInt32.self, capacity: 1 << 10)
  for i in 0..<32 { fetchWords[i * 6 + 2] = UInt32(textureSize - 1) | (UInt32(textureSize - 1) << 13) }

  var encoded: [(Int, MTLBuffer)] = []
  var used: [MTLResource] = [floats, system, boolLoop, fetch, texture2D, textureArray, texture3D, textureCube]
  let params = matches("fragment\\s+\\w+\\s+main0\\((.*?)\\)\\s*\\{", src).first?[1] ?? ""
  for p in matches("(\\w+)&\\s*\\w+\\s*\\[\\[buffer\\((\\d+)\\)\\]\\]", params) {
    let structName = p[1], index = Int(p[2])!
    let enc = fs.makeArgumentEncoder(bufferIndex: index)
    let ab = newBuffer(max(enc.encodedLength, 16))
    enc.setArgumentBuffer(ab, offset: 0)
    if let body = matches("struct \(structName)\\s*\\{(.*?)\\};", src).first?[1] {
      for m in matches("\\n\\s*([^;\\n]*?)\\s+\\*?\\s*(\\w+)\\s*\\[\\[id\\((\\d+)\\)\\]\\];", body) {
        let type = m[1], name = m[2], id = Int(m[3])!
        if name.hasSuffix("_pad") { continue }
        if type.contains("sampler") {
          enc.setSamplerState(sampler, index: id)
        } else if type.contains("texture2d_array") {
          enc.setTexture(textureArray, index: id)
        } else if type.contains("texture3d") {
          enc.setTexture(texture3D, index: id)
        } else if type.contains("texturecube") {
          enc.setTexture(textureCube, index: id)
        } else if type.contains("texture2d") {
          enc.setTexture(texture2D, index: id)
        } else if name.contains("float_constants") {
          enc.setBuffer(floats, offset: 0, index: id)
        } else if name.contains("system") {
          enc.setBuffer(system, offset: 0, index: id)
        } else if name.contains("bool_loop") {
          enc.setBuffer(boolLoop, offset: 0, index: id)
        } else if name.contains("fetch") {
          enc.setBuffer(fetch, offset: 0, index: id)
        } else {
          let b = newBuffer(1 << 14); enc.setBuffer(b, offset: 0, index: id); used.append(b)
        }
      }
    }
    encoded.append((index, ab))
  }
  let resources = used
  return Form(
    bind: { e, pattern in
      let bl = boolLoop.contents().bindMemory(to: UInt32.self, capacity: 40)
      bl[0] = pattern
      bl[4] = pattern
      for id in job.loopIds {
        var loop = [3, 0, 1, 0]
        if let literal = job.loopLiterals[String(id)] { loop = literal }
        bl[8 + id] = UInt32(loop[0] & 255) | UInt32(loop[1] & 255) << 8 | UInt32(loop[2] & 255) << 16
      }
      for (i, b) in encoded { e.setFragmentBuffer(b, offset: 0, index: i) }
      for r in resources { e.useResource(r, usage: .read, stages: .fragment) }
    },
    pipeline: { samples, format in makePipeline(library, fs, src, samples, format) })
}

func xenosForm() -> Form {
  var src = try! String(contentsOfFile: job.xenos, encoding: .utf8)
  var fields: [(attribute: String, value: String)] = []
  var seen = Set<String>()
  for f in matches("float4 i(\\w+) \\[\\[user\\((\\w+)\\)\\]\\]", src) where !seen.contains(f[2]) {
    seen.insert(f[2])
    if let register = job.interpolators[f[1]] {
      fields.append(("[[user(\(f[2]))]]", interpolatorExpression(register)))
    } else {
      fields.append(("[[user(\(f[2]))]]", "float4(0.0)"))
    }
  }
  src += vertexShader(fields)
  let library = compile(src, macros: ["__air__": NSNumber(value: 1)])
  let fs = try! library.makeFunction(name: "shaderMain", constantValues: MTLFunctionConstantValues())

  func table(_ id: MTLResourceID) -> MTLBuffer {
    let b = newBuffer(16 * 1024)
    let slots = b.contents().bindMemory(to: MTLResourceID.self, capacity: 2048)
    for i in 0..<2048 { slots[i] = id }
    return b
  }
  let tables = [table(texture2D.gpuResourceID), table(texture3D.gpuResourceID), table(textureCube.gpuResourceID),
                table(sampler.gpuResourceID)]
  let constants = newBuffer(1 << 16)
  let floats = constants.contents().bindMemory(to: Float.self, capacity: 1 << 14)
  for register in 0..<256 {
    for c in 0..<4 { floats[register * 4 + c] = constantValue(register, c) }
  }
  // Relative reads can land on a shader's literals, which D3D keeps in the register file.
  for (register, words) in job.literals {
    for c in 0..<4 { floats[Int(register)! * 4 + c] = Float(bitPattern: words[c]) }
  }
  let shared = newBuffer(1 << 16)
  var push: [UInt64] = [constants.gpuAddress, constants.gpuAddress, shared.gpuAddress]
  let resources: [MTLResource] = [texture2D, texture3D, textureCube, constants, shared]
  return Form(
    bind: { e, pattern in
      shared.contents().advanced(by: 256).bindMemory(to: UInt32.self, capacity: 1).pointee = pattern << 16
      let loops = shared.contents().advanced(by: 512).bindMemory(to: Int32.self, capacity: 128)
      for id in job.loopIds {
        loops[id * 4] = 3; loops[id * 4 + 1] = 0; loops[id * 4 + 2] = 1
      }
      for (i, b) in tables.enumerated() { e.setFragmentBuffer(b, offset: 0, index: i) }
      push.withUnsafeMutableBytes { e.setFragmentBytes($0.baseAddress!, length: 24, index: 8) }
      for r in resources { e.useResource(r, usage: .read, stages: .fragment) }
    },
    pipeline: { samples, format in makePipeline(library, fs, src, samples, format) })
}

func render(_ form: Form, _ pipeline: MTLRenderPipelineState, _ pattern: UInt32) -> [Float] {
  let ctd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba32Float, width: diffWidth, height: diffHeight, mipmapped: false)
  ctd.usage = .renderTarget; ctd.storageMode = .shared
  let color = device.makeTexture(descriptor: ctd)!
  let extra = (1..<4).map { _ in device.makeTexture(descriptor: ctd)! }
  let dtd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .depth32Float, width: diffWidth, height: diffHeight, mipmapped: false)
  dtd.usage = .renderTarget; dtd.storageMode = .private
  let depth = device.makeTexture(descriptor: dtd)!
  let rp = MTLRenderPassDescriptor()
  rp.colorAttachments[0].texture = color; rp.colorAttachments[0].loadAction = .clear; rp.colorAttachments[0].storeAction = .store
  for i in 1..<4 { rp.colorAttachments[i].texture = extra[i - 1]; rp.colorAttachments[i].loadAction = .clear; rp.colorAttachments[i].storeAction = .dontCare }
  rp.depthAttachment.texture = depth; rp.depthAttachment.loadAction = .clear; rp.depthAttachment.storeAction = .dontCare
  let cb = queue.makeCommandBuffer()!
  let e = cb.makeRenderCommandEncoder(descriptor: rp)!
  e.setRenderPipelineState(pipeline)
  let dsd = MTLDepthStencilDescriptor(); dsd.depthCompareFunction = .always; dsd.isDepthWriteEnabled = false
  e.setDepthStencilState(device.makeDepthStencilState(descriptor: dsd)!)
  form.bind(e, pattern)
  e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
  e.endEncoding()
  cb.commit(); cb.waitUntilCompleted()
  if cb.error != nil { print("{\"error\":\"gpu\"}"); exit(1) }
  var pixels = [Float](repeating: 0, count: diffWidth * diffHeight * 4)
  color.getBytes(&pixels, bytesPerRow: diffWidth * 16, from: MTLRegionMake2D(0, 0, diffWidth, diffHeight), mipmapLevel: 0)
  return pixels
}

func timeForm(_ form: Form, _ pipeline: MTLRenderPipelineState, _ pattern: UInt32) -> Double {
  let ctd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 1280, height: 720, mipmapped: false)
  ctd.textureType = .type2DMultisample; ctd.sampleCount = 2; ctd.usage = .renderTarget; ctd.storageMode = .private
  let colors = (0..<4).map { _ in device.makeTexture(descriptor: ctd)! }
  let dtd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .depth32Float, width: 1280, height: 720, mipmapped: false)
  dtd.textureType = .type2DMultisample; dtd.sampleCount = 2; dtd.usage = .renderTarget; dtd.storageMode = .private
  let depth = device.makeTexture(descriptor: dtd)!
  let dsd = MTLDepthStencilDescriptor(); dsd.depthCompareFunction = .always; dsd.isDepthWriteEnabled = false
  let dss = device.makeDepthStencilState(descriptor: dsd)!
  var best = Double.infinity
  for _ in 0..<6 {
    let rp = MTLRenderPassDescriptor()
    for i in 0..<4 { rp.colorAttachments[i].texture = colors[i]; rp.colorAttachments[i].loadAction = .clear; rp.colorAttachments[i].storeAction = .dontCare }
    rp.depthAttachment.texture = depth; rp.depthAttachment.loadAction = .clear; rp.depthAttachment.storeAction = .dontCare
    let cb = queue.makeCommandBuffer()!
    let e = cb.makeRenderCommandEncoder(descriptor: rp)!
    e.setRenderPipelineState(pipeline); e.setDepthStencilState(dss)
    form.bind(e, pattern)
    e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
    e.endEncoding()
    cb.commit(); cb.waitUntilCompleted()
    best = min(best, (cb.gpuEndTime - cb.gpuStartTime) * 1000)
  }
  return best
}

let emulator = emulatorForm(), xenos = xenosForm()
let emulatorDiff = emulator.pipeline(1, .rgba32Float), xenosDiff = xenos.pipeline(1, .rgba32Float)
var report: [String] = []
for pattern: UInt32 in [0, 0xFFFF, 0x5555] {
  let a = render(emulator, emulatorDiff, pattern), b = render(xenos, xenosDiff, pattern)
  var worst: Float = 0, over2 = 0, over8 = 0, nonZero = 0
  for i in 0..<diffWidth * diffHeight {
    var d: Float = 0
    for c in 0..<4 {
      let x = a[i * 4 + c], y = b[i * 4 + c]
      if x.isNaN != y.isNaN { d = 1; continue }
      if x.isNaN { continue }
      d = max(d, abs(min(max(x, 0), 1) - min(max(y, 0), 1)))
    }
    worst = max(worst, d)
    if d > 2.0 / 255 { over2 += 1 }
    if d > 8.0 / 255 { over8 += 1 }
    if a[i * 4] != 0 || a[i * 4 + 1] != 0 || a[i * 4 + 2] != 0 { nonZero += 1 }
  }
  let total = diffWidth * diffHeight
  report.append(String(format: "{\"bools\":%u,\"worst\":%.4f,\"over2\":%.5f,\"over8\":%.5f,\"lit\":%.3f}", pattern, worst,
                       Float(over2) / Float(total), Float(over8) / Float(total), Float(nonZero) / Float(total)))
}
var timing = ""
if ProcessInfo.processInfo.environment["TIME"] == "1" {
  let t = timeForm(emulator, emulator.pipeline(2, .rgba8Unorm), timedBools)
  let u = timeForm(xenos, xenos.pipeline(2, .rgba8Unorm), timedBools)
  timing = String(format: ",\"emulator_ms\":%.3f,\"xenos_ms\":%.3f", t, u)
}
print("{\"patterns\":[\(report.joined(separator: ","))]\(timing)}")
