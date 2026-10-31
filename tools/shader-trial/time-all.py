#!/usr/bin/env python3
"""Time every pixel shader that exists in both forms, one draw per pass, interleaved over three rounds.
Apple's GPUs skip fully covered pixels, so several opaque quads in one pass would only shade once."""
import json, re, statistics, subprocess
rows = [r for r in json.load(open("joined.json")) if r["msl"] and "-fs-" in r["msl"] and r["key"]]
ok = {l.split("\t")[0] for l in open("compiled.tsv") if l.split("\t")[1].strip() == "ok"}
rows = [r for r in rows if "ps_" + r["key"] in ok]
print(len(rows), "pairs", flush=True)
def run(binary, path):
    out = subprocess.run([binary, path, "1"], capture_output=True, text=True).stdout
    match = re.search(r"([\d.]+) ms/draw", out)
    return float(match.group(1)) if match else None
results = {i: {"xenos": [], "emulator": []} for i in range(len(rows))}
for round_number in range(3):
    for i, row in enumerate(rows):
        results[i]["xenos"].append(run("build/harness_xenos", f"translated/ps_{row['key']}.metal"))
        results[i]["emulator"].append(run("build/harness_emulator", row["msl"]))
    print("round", round_number + 1, flush=True)
with open("timing.tsv", "w") as f:
    f.write("key\tucode\txenos_ms\temulator_ms\n")
    for i, row in enumerate(rows):
        x = [v for v in results[i]["xenos"] if v]
        e = [v for v in results[i]["emulator"] if v]
        f.write(f"{row['key']}\t{row['ucode']}\t{min(x) if x else ''}\t{min(e) if e else ''}\n")
