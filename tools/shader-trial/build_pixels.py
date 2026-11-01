#!/usr/bin/env python3
"""Compile translated pixel shaders to metallibs, with a list of the texture slots each one reads.

usage: build_pixels.py <translated> <output folder>

Shadow mode has to fill a shader's descriptor table for every draw, and working out which of the 32 fetch slots a shader
reads from the ucode would mean parsing it, so the translated source says instead: each sampler has a define giving its
place in the table, and the shader body names the ones it uses. Pixel shaders aren't patched by D3D, so their name in
translated/ is the hash of their live code too.
"""
import glob
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

KINDS = {"Texture2D": 0, "Texture3D": 64, "TextureCube": 128}
DEFINE = re.compile(r"#define (\w+?)_(Texture2D|Texture3D|TextureCube)DescriptorIndex \(\*\(reinterpret_cast<device uint\*>"
                    r"\(g_PushConstants\.SharedConstants \+ (\d+)\)\)\)")


def slots_of(source):
    """The (slot, kind) pairs the shader's body reads. Kind is 0 for 2D, 1 for 3D and 2 for cube."""
    body = source[source.find("shaderMain"):]
    slots = set()
    for name, kind, offset in DEFINE.findall(source):
        if f"{name}_{kind}DescriptorIndex" in body:
            slots.add(((int(offset) - KINDS[kind]) // 4, list(KINDS).index(kind)))
    return sorted(slots)


def compile_one(job):
    source, target = job
    done = subprocess.run(["xcrun", "-sdk", "macosx", "metal", "-D__air__=1", source, "-o", target],
                          capture_output=True, text=True)
    return done.returncode


def main():
    translated, output = sys.argv[1:3]
    os.makedirs(output, exist_ok=True)
    manifest = []
    jobs = []
    for path in sorted(glob.glob(os.path.join(translated, "ps_*.metal"))):
        name = os.path.basename(path)[:-6]
        slots = slots_of(open(path, errors="ignore").read())
        manifest.append(" ".join([name[3:]] + [f"{slot} {kind}" for slot, kind in slots]))
        jobs.append((path, os.path.join(output, name + ".metallib")))
    with open(os.path.join(output, "pixel-manifest.txt"), "w") as out:
        out.write("\n".join(manifest) + "\n")
    with ThreadPoolExecutor(6) as pool:
        codes = list(pool.map(compile_one, jobs))
    print(json.dumps({"shaders": len(jobs), "failed": [jobs[i][0] for i, code in enumerate(codes) if code][:8]}))


if __name__ == "__main__":
    main()
