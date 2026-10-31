#!/usr/bin/env python3
"""Estimate what small translator fixes would buy, by patching the generated text. Not the real fix:
those belong in shader_recompiler.cpp. Writes to fixed/."""
import glob, os, re
os.makedirs("fixed", exist_ok=True)
HEAPS = ("constant Texture2DDescriptorHeap* g_Texture2DDescriptorHeap [[buffer(0)]],\n"
         "\tconstant Texture3DDescriptorHeap* g_Texture3DDescriptorHeap [[buffer(1)]],\n"
         "\tconstant TextureCubeDescriptorHeap* g_TextureCubeDescriptorHeap [[buffer(2)]],\n"
         "\tconstant SamplerDescriptorHeap* g_SamplerDescriptorHeap [[buffer(3)]],\n\t")
for path in glob.glob("translated/*.metal"):
    text = open(path).read()
    name = os.path.basename(path)
    if name.startswith("vs_") and "g_Texture2DDescriptorHeap," in text:
        text = text.replace("constant PushConstants& g_PushConstants [[buffer(8)]],", HEAPS + "constant PushConstants& g_PushConstants [[buffer(8)]],", 1)
    # Interpolators the game writes that the shared struct leaves out.
    for extra in re.findall(r"(?:input|output)\.([io](?:Normal|Color|Tangent|Binormal|Fog|TexCoord)\d+)", text):
        base = extra[1:]
        for side in "io":
            member = side + base
            if not re.search(r"float4 %s \[\[" % member, text):
                semantic = re.sub(r"(\D+)(\d+)", lambda m: m.group(1).upper() + m.group(2), base)
                text = text.replace("float4 %sColor0 [[user(COLOR0)]];" % side,
                                    "float4 %s [[user(%s)]];\n\tfloat4 %sColor0 [[user(COLOR0)]];" % (member, semantic, side), 1)
    # A vfetch with every component masked writes nothing.
    text = re.sub(r"^\s*r\d+\. = .*\.;\s*$", "", text, flags=re.M)
    # Fetch slots and bools the translator doesn't map. Vertex textures sit at s16 and up.
    used = set(re.findall(r"\b(s\d+)_(?:Texture\dD|TextureCube|Sampler)", text)) - set(re.findall(r"#define (s\d+)_", text))
    defines = ""
    for slot in sorted(used):
        for kind in ("Texture1D", "Texture2D", "Texture3D", "TextureCube", "Sampler"):
            defines += f"#define {slot}_{kind}DescriptorIndex 0u\n"
    for flag in set(re.findall(r"\bb(\d+)\b", text)):
        if not re.search(r"\bb%s\b.*(define|constant|bool)" % flag, text) and not re.search(r"#define b%s\b" % flag, text):
            defines += f"#define b{flag} 0u\n"
    if "g_Texture1DDescriptorHeap" in text and "Texture1DDescriptorHeap" not in text.split("[[vertex]]")[0].split("[[fragment]]")[0].replace("g_Texture1D", ""):
        text = text.replace("g_Texture1DDescriptorHeap", "g_Texture2DDescriptorHeap").replace("tfetch1D(", "tfetch2D(")
    marker = "[[vertex]]" if name.startswith("vs_") else "[[fragment]]"
    text = text.replace(marker, defines + marker, 1)
    open(os.path.join("fixed", name), "w").write(text)
