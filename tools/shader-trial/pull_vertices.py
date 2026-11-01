#!/usr/bin/env python3
"""Make translated vertex shaders read guest memory themselves, and compile each to a metallib.

usage: pull_vertices.py <extracted> <translated> <output folder>

XenosRecomp leaves vertex input to a Metal vertex descriptor, which can't read big-endian data. A
shader's fetch formats aren't known here either: most shipped shaders hold blanks that the game's D3D
fills in when it binds them, so the live ucode is the only source. So each shader gets one
pullElement call per vertex element, and the game hands it a table built from the live ucode, which
manifest.txt says where to read. XenosRecomp drops r0's value, which is the vertex index on the Xenos,
so that's put back: some shaders work out a quad's corners from it. It drops a fetch's index register
as well, and some shaders fetch with the index divided by four, which is one vertex to a quad. So
each fetch uses the register its ucode names, once the shader has got to it.
"""
import glob
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

USAGES = {0: "Position", 1: "BlendWeight", 2: "BlendIndices", 3: "Normal", 4: "PointSize", 5: "TexCoord",
          6: "Tangent", 7: "Binormal", 8: "TessFactor", 9: "PositionT", 10: "Color", 11: "Fog", 12: "Depth",
          13: "Sample"}

EXEC_OPCODES = {1, 2, 3, 4, 5, 6, 13, 14}

# A draw of screen pixels skips the viewport, which Metal can't, so shadow mode sets these to turn pixels into clip space.
SCREEN_DEFINES = """
#define g_ScreenScale (*(reinterpret_cast<device float2*>(g_PushConstants.SharedConstants + 280)))
#define g_ScreenOffset (*(reinterpret_cast<device float2*>(g_PushConstants.SharedConstants + 288)))
#define g_DepthOffset (*(reinterpret_cast<device float*>(g_PushConstants.SharedConstants + 296)))
"""

PREAMBLE = """
struct PullElement {
    uint packed;  // slot, stride << 8, format << 16, then signed, integer, no-zero and reads at bits 24-27.
    int offset;
    float scale;
    uint select;  // Where each lane of the result comes from, 3 bits each: x, y, z, w, 0 or 1.
};
static inline float pullLane(float4 v, uint select) {
    return select < 4 ? v[select] : select == 5 ? 1.0 : 0.0;
}
static inline uint guestWord(const device uint* guest, uint address, uint endian) {
    uint value = guest[address & 0x07FFFFFF];
    if (endian == 1) {
        value = ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF);
    } else if (endian == 2) {
        value = (value << 24) | ((value & 0xFF00) << 8) | ((value >> 8) & 0xFF00) | (value >> 24);
    } else if (endian == 3) {
        value = (value << 16) | (value >> 16);
    }
    return value;
}
static inline float pullField(uint word, uint offset, uint width, bool isSigned, bool isInteger, bool noZero) {
    float value = isSigned ? float(extract_bits(int(word), offset, width)) : float(extract_bits(word, offset, width));
    if (isInteger) {
        return value;
    }
    if (!isSigned) {
        return value / float((1u << width) - 1u);
    }
    float limit = float((1u << (width - 1u)) - 1u);
    return noZero ? (value + 0.5) / (limit + 0.5) : max(value / limit, -1.0);
}
static inline float pullWhole(uint word, bool isSigned, bool isInteger, bool noZero) {
    float value = isSigned ? float(int(word)) : float(word);
    if (isInteger) {
        return value;
    }
    if (!isSigned) {
        return value / 4294967295.0;
    }
    return noZero ? (value + 0.5) / 2147483647.5 : max(value / 2147483647.0, -1.0);
}
static inline float2 pullHalves(uint word) {
    return float2(float(as_type<half>(ushort(word & 0xFFFF))), float(as_type<half>(ushort(word >> 16))));
}
// Follows SpirvShaderTranslator::ProcessVertexFetchInstruction.
static float4 pullElement(const device uint* guest, constant uint* fetch, constant PullElement& element, uint vertexIndex) {
    uint packed = element.packed;
    if ((packed & (1u << 27)) == 0) {
        return float4(0.0);
    }
    uint slot = packed & 0xFF, stride = (packed >> 8) & 0xFF, format = (packed >> 16) & 0x3F;
    bool isSigned = (packed & (1u << 24)) != 0, isInteger = (packed & (1u << 25)) != 0, noZero = (packed & (1u << 26)) != 0;
    uint endian = fetch[slot * 2 + 1] & 3;
    uint address = uint(int((fetch[slot * 2] >> 2) & 0x07FFFFFF) + int(vertexIndex) * int(stride) + element.offset);
    uint w0 = guestWord(guest, address, endian);
    float4 v = float4(0.0);
    switch (format) {
        case 6:
            v = float4(pullField(w0, 0, 8, isSigned, isInteger, noZero), pullField(w0, 8, 8, isSigned, isInteger, noZero),
                       pullField(w0, 16, 8, isSigned, isInteger, noZero), pullField(w0, 24, 8, isSigned, isInteger, noZero));
            break;
        case 7:
            v = float4(pullField(w0, 0, 10, isSigned, isInteger, noZero), pullField(w0, 10, 10, isSigned, isInteger, noZero),
                       pullField(w0, 20, 10, isSigned, isInteger, noZero), pullField(w0, 30, 2, isSigned, isInteger, noZero));
            break;
        case 16:
            v = float4(pullField(w0, 0, 11, isSigned, isInteger, noZero), pullField(w0, 11, 11, isSigned, isInteger, noZero),
                       pullField(w0, 22, 10, isSigned, isInteger, noZero), 0.0);
            break;
        case 17:
            v = float4(pullField(w0, 0, 10, isSigned, isInteger, noZero), pullField(w0, 10, 11, isSigned, isInteger, noZero),
                       pullField(w0, 21, 11, isSigned, isInteger, noZero), 0.0);
            break;
        case 25:
            v = float4(pullField(w0, 0, 16, isSigned, isInteger, noZero), pullField(w0, 16, 16, isSigned, isInteger, noZero), 0.0, 0.0);
            break;
        case 26: {
            uint w1 = guestWord(guest, address + 1, endian);
            v = float4(pullField(w0, 0, 16, isSigned, isInteger, noZero), pullField(w0, 16, 16, isSigned, isInteger, noZero),
                       pullField(w1, 0, 16, isSigned, isInteger, noZero), pullField(w1, 16, 16, isSigned, isInteger, noZero));
            break;
        }
        case 31:
            v.xy = pullHalves(w0);
            break;
        case 32:
            v.xy = pullHalves(w0);
            v.zw = pullHalves(guestWord(guest, address + 1, endian));
            break;
        case 33:
            v.x = pullWhole(w0, isSigned, isInteger, noZero);
            break;
        case 34:
            v.x = pullWhole(w0, isSigned, isInteger, noZero);
            v.y = pullWhole(guestWord(guest, address + 1, endian), isSigned, isInteger, noZero);
            break;
        case 35:
            v.x = pullWhole(w0, isSigned, isInteger, noZero);
            v.y = pullWhole(guestWord(guest, address + 1, endian), isSigned, isInteger, noZero);
            v.z = pullWhole(guestWord(guest, address + 2, endian), isSigned, isInteger, noZero);
            v.w = pullWhole(guestWord(guest, address + 3, endian), isSigned, isInteger, noZero);
            break;
        case 36:
            v.x = as_type<float>(w0);
            break;
        case 37:
            v.x = as_type<float>(w0);
            v.y = as_type<float>(guestWord(guest, address + 1, endian));
            break;
        case 38:
        case 57:
            v.x = as_type<float>(w0);
            v.y = as_type<float>(guestWord(guest, address + 1, endian));
            v.z = as_type<float>(guestWord(guest, address + 2, endian));
            if (format == 38) {
                v.w = as_type<float>(guestWord(guest, address + 3, endian));
            }
            break;
        default:
            break;
    }
    v *= element.scale;
    uint select = element.select;
    return float4(pullLane(v, select & 7), pullLane(v, (select >> 3) & 7), pullLane(v, (select >> 6) & 7), pullLane(v, (select >> 9) & 7));
}
"""


def masked_code(code):
    """The ucode with what binding a shader to a vertex declaration patches blanked out. The game's D3D writes
    each vertex fetch's format, offset, stride and fetch slot into the shader when it binds it, so most shipped
    shaders hold none of these and only the live copy does. src/native/shadow.cpp does the same to live ucode."""
    words = list(struct.unpack(">%dI" % (len(code) // 4), code))
    first_instruction = len(words) // 3
    pair = 0
    while pair * 3 < len(words) and pair < first_instruction:
        low, middle, high = words[pair * 3 : pair * 3 + 3]
        for cf_low, cf_high in ((low, middle & 0xFFFF), (((middle >> 16) | (high << 16)) & 0xFFFFFFFF, high >> 16)):
            if (cf_high >> 12) & 0xF not in EXEC_OPCODES:
                continue
            address, count, sequence = cf_low & 0xFFF, (cf_low >> 12) & 7, (cf_low >> 16) & 0xFFF
            first_instruction = min(first_instruction, address)
            for i in range(count):
                at = (address + i) * 3
                if (sequence >> (i * 2)) & 1 and at + 2 < len(words) and words[at] & 0x1F == 0:
                    words[at] &= 0xC00FFFFF
                    words[at + 1] &= 0x80000000
                    words[at + 2] = 0
        pair += 1
    return struct.pack(">%dI" % len(words), *words)


def elements_of(path):
    data = open(path, "rb").read()
    virtual_size = struct.unpack_from(">I", data, 4)[0]
    shader_offset = struct.unpack_from(">I", data, 24)[0]
    physical_offset, size = struct.unpack_from(">II", data, shader_offset)
    code = data[virtual_size + physical_offset : virtual_size + physical_offset + size]
    field18, count = struct.unpack_from(">II", data, shader_offset + 24)
    elements = []
    for i in range(count):
        value = struct.unpack_from(">I", data, shader_offset + 36 + (field18 + i) * 4)[0]
        address = value & 0xFFF
        word0, word1 = struct.unpack_from(">II", code, address * 3 * 4)
        elements.append({"address": address, "usage": (value >> 12) & 0xF, "index": (value >> 16) & 0xF, "swizzle": word1 & 0xFFF,
                         "register": (word0 >> 5) & 0x3F, "component": word0 >> 30, "rounded": (word1 >> 15) & 1,
                         "mini": (word1 >> 30) & 1})
    elements.sort(key=lambda e: e["address"])
    # A mini fetch has no index of its own.
    for previous, element in zip(elements, elements[1:]):
        if element["mini"]:
            element.update({key: previous[key] for key in ("register", "component", "rounded")})
    return code, elements


def rewrite(source, elements):
    source = re.sub(r" \[\[attribute\(\d+\)\]\]", "", source)
    # Declared as uint4 for a vertex descriptor's integer format. The fetch gives the same numbers as floats.
    source = source.replace("uint4 iBlendIndices", "float4 iBlendIndices")
    a = "\tVertexShaderInput input [[stage_in]]\n#else"
    b = ("\tuint vertex_id [[vertex_id]],\n"
         "\tconst device uint* g_Guest [[buffer(9)]],\n"
         "\tconstant uint* g_Fetch [[buffer(10)]],\n"
         "\tconstant PullElement* g_Elements [[buffer(11)]]\n#else")
    if a not in source:
        raise ValueError("no stage_in")
    source = source.replace(a, b)
    a = "\tInterpolators output = Interpolators{};\n"
    if source.count(a) != 1:
        raise ValueError("no output line")
    start = source.index(a) + len(a)
    first_read = re.compile(r"\binput\.i").search(source, start)
    r0_changed = first_read and re.search(r"^\s*r0(\.[xyzw]+)? = ", source[start:first_read.start()], re.M)
    pulls = []
    for i, element in enumerate(elements):
        name = f"i{USAGES[element['usage']]}{element['index']}"
        component = f"r{element['register']}.{'xyzw'[element['component']]}"
        index = "vertex_id" if component == "r0.x" and not r0_changed else f"uint(floor({component}{' + 0.5' if element['rounded'] else ''}))"
        pulls.append(f"\tinput.{name} = as_type<decltype(input.{name})>(pullElement(g_Guest, g_Fetch, g_Elements[{i}], {index}));\n")
    pulls = "#ifdef __air__\n" + "".join(pulls) + "#endif\n"
    if first_read:
        line = source.rindex("\n", 0, first_read.start()) + 1
        source = source[:line] + pulls + source[line:]
    source = source.replace(a, a + "\tVertexShaderInput input = VertexShaderInput{};\n")
    a = "\toutput.oPos.xy += g_HalfPixelOffset * output.oPos.w;\n"
    if source.count(a) != 1:
        raise ValueError("no position line")
    source = source.replace(a, "#ifdef __air__\n\toutput.oPos.xy = output.oPos.xy * g_ScreenScale + g_ScreenOffset * output.oPos.w;\n\toutput.oPos.z += g_DepthOffset * output.oPos.w;\n#else\n" + a + "#endif\n")
    a = "\tfloat4 r0 = 0.0;\n"
    if source.count(a) != 1:
        raise ValueError("no r0")
    source = source.replace(a, "#ifdef __air__\n\tfloat4 r0 = float4(float(vertex_id), 0.0, 0.0, 0.0);\n#else\n" + a + "#endif\n")
    # The loaders go after the shared header, before the shader's own input struct.
    a = "struct VertexShaderInput\n"
    if source.count(a) != 1:
        raise ValueError("no input struct")
    return source.replace(a, "#ifdef __air__" + SCREEN_DEFINES + PREAMBLE + "#endif\n" + a)


def compile_one(job):
    source, target, log = job
    done = subprocess.run(["xcrun", "-sdk", "macosx", "metal", "-D__air__=1", source, "-o", target],
                          capture_output=True, text=True)
    open(log, "w").write(done.stderr)
    return done.returncode


def main():
    extracted, translated, output = sys.argv[1:4]
    os.makedirs(output, exist_ok=True)
    work = os.path.join(output, "source")
    os.makedirs(work, exist_ok=True)
    jobs = []
    manifest = {}
    report = {"bad": {}, "same_masked_key": 0}
    for path in sorted(glob.glob(os.path.join(translated, "vs_*.metal"))):
        name = os.path.basename(path)[:-6]
        try:
            code, elements = elements_of(os.path.join(extracted, name + ".bin"))
            source = rewrite(open(path).read(), elements)
        except (ValueError, struct.error, KeyError) as error:
            report["bad"][name] = str(error)
            continue
        key = hashlib.sha256(masked_code(code)).hexdigest()[:16]
        if key in manifest:
            report["same_masked_key"] += 1
            continue
        # The address, then the swizzle the translated shader was built with. Binding rewrites it for the live format.
        manifest[key] = [value for element in elements for value in (element["address"], element["swizzle"])]
        target = os.path.join(work, f"vs_{key}.metal")
        open(target, "w").write(source)
        jobs.append((target, os.path.join(output, f"vs_{key}.metallib"), os.path.join(work, f"vs_{key}.log")))
    with open(os.path.join(output, "manifest.txt"), "w") as out:
        for key, addresses in sorted(manifest.items()):
            out.write(" ".join([key] + [str(value) for value in addresses]) + "\n")
    with ThreadPoolExecutor(6) as pool:
        codes = list(pool.map(compile_one, jobs))
    report["compiled"] = codes.count(0)
    report["failed"] = [os.path.basename(job[0]) for job, code in zip(jobs, codes) if code]
    json.dump(report, open(os.path.join(output, "report.json"), "w"), indent=1)
    print(json.dumps({k: (v if not isinstance(v, (list, dict)) or len(v) < 12 else len(v)) for k, v in report.items()}))


if __name__ == "__main__":
    main()
