#!/usr/bin/env python3
"""Works out, for each shader in both forms, which guest register feeds what, then runs pixel_diff.

The emulator packs the float constants a shader uses, in register order. XenosRecomp names them and
keeps the shader's literals inline, so its text gives the register list the emulator's buffer needs.
"""
import json, os, re, subprocess, sys

time_too = len(sys.argv) > 1 and sys.argv[1] == "time"
os.makedirs("jobs", exist_ok=True)
rows = [r for r in json.load(open("joined.json")) if r["msl"] and "-fs-" in r["msl"] and r["key"]]
done, results, skipped = set(), {}, {}

for row in rows:
    key = row["key"]
    if key in done:
        continue
    done.add(key)
    xenos_path = f"translated/ps_{key}.metal"
    text = open(xenos_path).read()
    head, body = text.rsplit("shaderMain(", 1)
    emulator = open(row["msl"]).read()

    arrays = {m[0]: int(m[1]) for m in re.findall(
        r"#define (\w+)\(INDEX\) selectWrapper\(\(INDEX\) < \d+, \(\*\(reinterpret_cast<device float4\*>\(g_PushConstants\.PixelShaderConstants \+ \((\d+) \+ min", head)}
    singles = {m[0]: int(m[1]) // 16 for m in re.findall(
        r"#define (\w+) \(\*\(reinterpret_cast<device float4\*>\(g_PushConstants\.PixelShaderConstants \+ (\d+)\)\)\)", head)}

    literals = {}
    for register, words in re.findall(r"float4 c(\d+) = as_type<float4>\(uint4\(([^)]*)\)\);", body):
        literals[register] = [int(w, 16) for w in words.split(", ")]
    # What the instructions read: the literal declarations themselves don't count.
    code = re.sub(r"^\s*float4 c\d+ = .*$", "", body, flags=re.M)

    registers, dynamic = set(), False
    for register in re.findall(r"\bc(\d+)\b", code):
        registers.add(int(register))
    for name, base in arrays.items():
        for index, rest in re.findall(r"\b%s\((\d+)( \+ a[0L])?\)" % name, code):
            registers.add(base + int(index))
            dynamic |= bool(rest)
    for name, register in singles.items():
        if re.search(r"\b%s\b" % name, code):
            registers.add(register)

    interpolators = {m[1]: int(m[0]) for m in re.findall(r"float4 r(\d+) = input\.i(\w+);", body)}
    loop_literals = {m[0]: [int(x) for x in m[1].split(", ")] for m in re.findall(r"int4 i(\d+) = int4\(([^)]*)\);", body)}
    loop_ids = sorted({int(i) for i in re.findall(r"\bi(\d+)\.x", body)})
    packed = re.search(r"float4 float_constants\[(\d+)\]", emulator)
    count = int(packed.group(1)) if packed else 0

    def run(float_registers):
        job = {"emulator": row["msl"], "xenos": xenos_path, "floatRegisters": float_registers, "literals": literals,
               "interpolators": interpolators, "loopLiterals": loop_literals, "loopIds": loop_ids}
        path = f"jobs/{key}.json"
        json.dump(job, open(path, "w"))
        env = dict(os.environ, TIME="1") if time_too else os.environ
        try:
            out = subprocess.run(["build/pixel_diff", path], capture_output=True, text=True, timeout=60, env=env)
            return json.loads(out.stdout.strip().split("\n")[-1])
        except Exception as error:
            return {"error": str(error)[:100]}

    ordered = sorted(registers)
    if dynamic or re.search(r"float_constants\[[^0-9]", emulator):
        # Relative reads need the whole register file, in register order.
        result = run(list(range(count)))
        note = "relative constants"
    elif count == len(ordered):
        result = run(ordered)
        note = ""
    elif count == len(ordered) + 1:
        # The emulator counts one constant only a discarded result reads. Its place is found by
        # trying each one: the constants are random, so a wrong place can't match by chance.
        candidates = [run(ordered[:i] + [999] + ordered[i:]) for i in range(len(ordered) + 1)]
        candidates = [c for c in candidates if "patterns" in c]
        result = min(candidates, key=lambda c: sum(p["over2"] for p in c["patterns"]))
        note = "one unused constant"
    else:
        skipped[key] = f"constants {count} against {len(ordered)}"
        continue
    result["ucode"] = row["ucode"]
    result["note"] = note
    results[key] = result
    if len(results) % 20 == 0:
        print(len(results), flush=True)

json.dump({"results": results, "skipped": skipped}, open("pixels.json", "w"), indent=1)
print("compared", len(results), "skipped", len(skipped), skipped)
