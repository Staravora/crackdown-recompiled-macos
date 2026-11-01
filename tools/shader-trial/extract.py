#!/usr/bin/env python3
"""Pull the game's shipped shader containers into one file each, with virtual and physical parts joined.

usage: extract.py <game folder> <output folder> [executable image]

XenosRecomp expects a container's physical section right after its virtual one. That holds in
prebuilt_shadercache.dat. The .bin, .spk and .bsh packs keep all virtual sections first, then the
physical ones, so the two get joined here. Output is keyed by a hash of the
ucode, because the same shader appears in several packs. The HUD and menus also use a few shaders
compiled into the executable, which is compressed on disk, so a copy of its image taken from memory
is a source too: run the game with CRACKDOWN_DUMP_IMAGE set to a path. Its containers with no
constant table are D3D's own helpers, which the translator can't read, so they're left out.
"""
import hashlib
import json
import os
import struct
import sys

MAGIC_MASK = 0xFFFFFF00
MAGIC = 0x102A1100


def containers(data):
    i = 0
    while i < len(data) - 36:
        if struct.unpack_from(">I", data, i)[0] & MAGIC_MASK == MAGIC:
            yield i, struct.unpack_from(">9I", data, i)
        i += 4


def ucode(virtual, physical):
    """The instruction words inside a container's physical section."""
    shader_offset = struct.unpack_from(">I", virtual, 24)[0]
    offset, size = struct.unpack_from(">II", virtual, shader_offset)
    return physical[offset : offset + size]


def inline_shaders(data):
    for offset, fields in containers(data):
        virtual_size, physical_size = fields[1], fields[2]
        yield data[offset : offset + virtual_size], data[offset + virtual_size : offset + virtual_size + physical_size]


def packed_shaders(data):
    """A 12-byte index entry per shader holds its physical offset. boundshaders.bsh has one count and
    an index size where the others have two counts, and its virtual total leaves the index out."""
    first, second, virtual_total, _ = struct.unpack_from(">4I", data, 0)
    bound = second == first * 12
    count = first if bound else first + second
    physical = 16 + (second if bound else 0) + virtual_total
    found = list(containers(data[:physical]))
    assert len(found) == count, (len(found), count)
    for i, (offset, fields) in enumerate(found):
        physical_offset = struct.unpack_from(">I", data, 16 + 12 * i + 8)[0]
        start = physical + physical_offset
        yield data[offset : offset + fields[1]], data[start : start + fields[2]]


def main():
    game, out = sys.argv[1:3]
    image = sys.argv[3] if len(sys.argv) > 3 else None
    os.makedirs(out, exist_ok=True)
    sources = {
        "ScreenRenderer/prebuilt_shadercache.dat": inline_shaders,
        "ScreenRenderer/RTWUberShader.bin": packed_shaders,
        "vfx_shaders.spk": packed_shaders,
        "boundshaders.bsh": packed_shaders,
    }
    if image:
        sources[image] = inline_shaders
    index = {}
    for name, reader in sources.items():
        data = open(os.path.join(game, name), "rb").read()
        count = fresh = 0
        for virtual, physical in reader(data):
            if not struct.unpack_from(">I", virtual, 16)[0]:
                continue
            code = ucode(virtual, physical)
            key = hashlib.sha256(code).hexdigest()[:16]
            count += 1
            if key in index:
                index[key]["sources"].append(name)
                continue
            fresh += 1
            kind = "vs" if struct.unpack_from(">I", virtual, 0)[0] & 1 else "ps"
            with open(os.path.join(out, f"{kind}_{key}.bin"), "wb") as f:
                f.write(virtual + physical)
            index[key] = {"kind": kind, "dwords": len(code) // 4, "sources": [name]}
        print(f"{name}: {count} shaders, {fresh} new")
    vs = sum(1 for entry in index.values() if entry["kind"] == "vs")
    print(f"unique: {len(index)} ({vs} VS, {len(index) - vs} PS)")
    with open(os.path.join(out, "index.json"), "w") as f:
        json.dump(index, f, indent=1)


if __name__ == "__main__":
    main()
