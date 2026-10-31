#!/bin/zsh
# Phase 1 shader trial: build XenosRecomp's translator (PR 22, commit 4906992, MIT, from
# github.com/hedge-dev/XenosRecomp) with this project's patch and without its HLSL compiler,
# translate every extracted shader to
# Metal, and compile each with Apple's metal compiler. Results land in compiled.tsv; nothing touches
# the game or the app. Rerunnable.
set -u
cd "${0:A:h}"
R=XenosRecomp

rm -f done.txt
git -C $R checkout -q -- . || exit 1
git -C $R checkout -q 4906992 || exit 1
git -C $R apply ../xenosrecomp-crackdown.patch || exit 1
git -C $R submodule update --init --depth 1 thirdparty/fmt thirdparty/xxHash || exit 1

rm -rf translated
mkdir -p stubs build translated
: > stubs/dxcapi.h
: > stubs/smolv.h
: > stubs/zstd.h
flags=(-std=c++20 -O1 -g -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -fms-extensions -Wno-microsoft
       -Istubs -I$R/XenosRecomp -I$R/thirdparty/fmt/include -I$R/thirdparty/xxHash
       -DFMT_HEADER_ONLY=1 -DXXH_INLINE_ALL -include $R/XenosRecomp/pch.h)
clang++ $flags -c $R/XenosRecomp/shader_recompiler.cpp -o build/shader_recompiler.o || exit 1
clang++ $flags -c trial_main.cpp -o build/trial_main.o || exit 1
clang++ build/shader_recompiler.o build/trial_main.o -o build/xenos_one || exit 1

# One process per shader: a crash in the translator is that shader's failure.
for bin in extracted/*.bin; do
  name=${bin:t:r}
  ./build/xenos_one $bin translated/$name.metal $R/XenosRecomp/shader_common.h \
    > translated/$name.translate.txt 2>&1 &
  while (( $(jobs -r | wc -l) >= 6 )); do sleep 0.05; done
done
wait
zsh compile-all.sh
python3 - <<'EOF'
import collections
rows = [line.rstrip("\n").split("\t") for line in open("compiled.tsv")]
print(collections.Counter(row[1] for row in rows))
for kind in ("vs", "ps"):
    subset = [row for row in rows if row[0].startswith(kind)]
    print(kind, len(subset), "ok", sum(row[1] == "ok" for row in subset))
for message, count in collections.Counter(row[2] for row in rows if row[1] != "ok").most_common(12):
    print(count, message)
EOF

date > done.txt
