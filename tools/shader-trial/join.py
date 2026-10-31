#!/usr/bin/env python3
"""Line each shipped shader up with the emulator's Metal for it. spv-map holds SPIR-V by ucode hash,
msl-dump holds the same SPIR-V by MoltenVK's hash, and the emulator's .xsh ties ucode hash to ucode."""
import glob, hashlib, json, os, struct
XSH = "../../Crackdown Data/user_data/cache/shaders/shareable/4D5307DC.xsh"
data = open(XSH, "rb").read()
offset = 8
ucode_by_hash = {}
while offset + 12 <= len(data):
    hash_value, count = struct.unpack_from("<QI", data, offset)
    count &= 0xFFFF
    start = offset + 12
    ucode_by_hash["%016X" % hash_value] = data[start : start + count * 4]
    offset = start + count * 4
print("runtime shaders in cache", len(ucode_by_hash))
index = json.load(open("extracted/index.json"))
key_by_sha = {}
for key in index:
    key_by_sha[key] = key
def shipped_key(code):
    for candidate in (code, b"".join(code[i:i+4][::-1] for i in range(0, len(code), 4))):
        key = hashlib.sha256(candidate).hexdigest()[:16]
        if key in index:
            return key
    return None
msl_by_spv = {}
for path in glob.glob("msl-dump/shader-*s-*.spv"):
    msl_by_spv[hashlib.sha256(open(path, "rb").read()).hexdigest()] = path[:-4] + ".metal"
rows = []
for path in sorted(glob.glob("spv-map/*.spv")):
    ucode_hash, modification = os.path.basename(path)[:-4].split("-")
    msl = msl_by_spv.get(hashlib.sha256(open(path, "rb").read()).hexdigest())
    key = shipped_key(ucode_by_hash.get(ucode_hash, b""))
    rows.append({"ucode": ucode_hash, "modification": modification, "msl": msl, "key": key})
json.dump(rows, open("joined.json", "w"), indent=1)
pixel = [r for r in rows if r["msl"] and "-fs-" in r["msl"]]
vertex = [r for r in rows if r["msl"] and "-vs-" in r["msl"]]
print("modules", len(rows), "with MSL", len(pixel) + len(vertex), "pixel", len(pixel), "vertex", len(vertex))
print("pixel with shipped key", sum(1 for r in pixel if r["key"]), "unique", len({r["key"] for r in pixel if r["key"]}))
print("vertex with shipped key", sum(1 for r in vertex if r["key"]))
ok = {l.split("\t")[0] for l in open("compiled.tsv") if l.split("\t")[1] == "ok"}
print("pixel with compiled translation", sum(1 for r in pixel if r["key"] and "ps_" + r["key"] in ok))
print("B20B230BE9FD2568", [r for r in rows if r["ucode"] == "B20B230BE9FD2568"][:2])
