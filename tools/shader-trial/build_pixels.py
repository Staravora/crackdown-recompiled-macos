#!/usr/bin/env python3
"""Compile translated pixel shaders to metallibs, with a list of the texture slots each one reads.

usage: build_pixels.py <translated> <output folder>

Shadow mode has to fill a shader's descriptor table for every draw, and working out which of the 32 fetch slots a shader
reads from the ucode would mean parsing it, so the translated source says instead: each sampler has a define giving its
place in the table, and the shader body names the ones it uses. Pixel shaders aren't patched by D3D, so their name in
translated/ is the hash of their live code too.

Scene shaders read the shadow mask at pixel position / (1280, 484), the size of one tile, which is
right on the Xenos, where positions restart at each tile. Here one pass draws the whole picture and
the mask is joined up to match, so that constant becomes 1 / 720. screen-shaders.txt lists those
shaders, because even when one is drawn for a single tile it has to read the joined-up textures.

A fetch constant can ask for a channel to come back as value * 2 - 1, which no Metal format does and
the translator doesn't do. The normal buffer depends on it. So the top four bits of a texture's
descriptor say which channels want it, and the 2D fetches apply it.

Inputs are joined to vertex outputs by position, as pull_vertices.py describes.
"""
import glob
import json
import os
import re
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

from pull_vertices import relink

KINDS = {"Texture2D": 0, "Texture3D": 64, "TextureCube": 128}
DEFINE = re.compile(r"#define (\w+?)_(Texture2D|Texture3D|TextureCube)DescriptorIndex \(\*\(reinterpret_cast<device uint\*>"
                    r"\(g_PushConstants\.SharedConstants \+ (\d+)\)\)\)")


BIAS = """static inline float4 biased(float4 value, uint descriptor) {
    return select(value, value * 2.0 - 1.0, bool4((descriptor & (1u << 28)) != 0, (descriptor & (1u << 29)) != 0,
                                                  (descriptor & (1u << 30)) != 0, (descriptor & (1u << 31)) != 0));
}

"""
FETCH_2D = re.compile(r"return (texture\.sample\(sampler, texCoord \+ offset / \(float2\)getTexture2DDimensions\(texture\)[^;]*\));")
INPUT = re.compile(r"float4 r(\d+) = input\.i(TexCoord|Color|Normal)(\d+);")
ONE_OVER_TILE_HEIGHT = "0x3B0767AB"
ONE_OVER_PICTURE_HEIGHT = f"0x{struct.unpack('<I', struct.pack('<f', 1 / 720))[0]:08X}"


def slots_of(source):
    """The (slot, kind) pairs the shader's body reads. Kind is 0 for 2D, 1 for 3D and 2 for cube."""
    body = source[source.find("shaderMain"):]
    slots = set()
    for name, kind, offset in DEFINE.findall(source):
        if f"{name}_{kind}DescriptorIndex" in body:
            slots.add(((int(offset) - KINDS[kind]) // 4, list(KINDS).index(kind)))
    return sorted(slots)


def patched(source):
    source = source.replace(ONE_OVER_TILE_HEIGHT, ONE_OVER_PICTURE_HEIGHT)
    source = source.replace("texture2d<float> texture = textureHeap[resourceDescriptorIndex].tex;",
                            "texture2d<float> texture = textureHeap[resourceDescriptorIndex & 0x0FFFFFFF].tex;")
    source = FETCH_2D.sub(r"return biased(\1, resourceDescriptorIndex);", source)
    a = "uint2 getTexture2DDimensions(texture2d<float> texture)"
    source = source.replace(a, BIAS + a, 1)
    inputs = sorted((int(register), name.upper() + index) for register, name, index in INPUT.findall(source))
    return relink(source, [name for _, name in inputs])


def compile_one(job):
    source, target = job
    done = subprocess.run(["xcrun", "-sdk", "macosx", "metal", "-D__air__=1", source, "-o", target],
                          capture_output=True, text=True)
    return done.returncode


def main():
    translated, output = sys.argv[1:3]
    work = os.path.join(output, "source")
    os.makedirs(work, exist_ok=True)
    manifest = []
    screen = []
    jobs = []
    for path in sorted(glob.glob(os.path.join(translated, "ps_*.metal"))):
        name = os.path.basename(path)[:-6]
        source = open(path, errors="ignore").read()
        slots = slots_of(source)
        if ONE_OVER_TILE_HEIGHT in source[source.find("shaderMain"):]:
            screen.append(name[3:])
        manifest.append(" ".join([name[3:]] + [f"{slot} {kind}" for slot, kind in slots]))
        target = os.path.join(work, name + ".metal")
        open(target, "w").write(patched(source))
        jobs.append((target, os.path.join(output, name + ".metallib")))
    with open(os.path.join(output, "pixel-manifest.txt"), "w") as out:
        out.write("\n".join(manifest) + "\n")
    with open(os.path.join(output, "screen-shaders.txt"), "w") as out:
        out.write("\n".join(screen) + "\n")
    with ThreadPoolExecutor(6) as pool:
        codes = list(pool.map(compile_one, jobs))
    print(json.dumps({"shaders": len(jobs), "failed": [jobs[i][0] for i, code in enumerate(codes) if code][:8]}))


if __name__ == "__main__":
    main()
