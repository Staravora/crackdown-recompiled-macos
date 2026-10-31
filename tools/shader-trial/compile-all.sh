#!/bin/zsh
# Second half of the trial: compile the translated shaders with Apple's compiler. This toolchain no
# longer predefines __air__, which the generated code branches on, so it's passed here.
zmodload zsh/datetime
cd "${0:A:h}"
one() {
  local source=$1 name=${1:t:r} out=translated/${1:t:r}
  local start=$EPOCHREALTIME
  if xcrun -sdk macosx metal -D__air__=1 -c $source -o $out.air 2> $out.compile.txt; then
    printf '%s\tok\t%.0f\n' $name $(( ($EPOCHREALTIME - start) * 1000 ))
  else
    printf '%s\tcompile-failed\t%s\n' $name "$(grep -m1 'error:' $out.compile.txt | sed -E 's/^[^ ]+ error: //' | cut -c1-140)"
  fi
}
: > compiled.tsv
for source in translated/*.metal; do
  one $source >> compiled.tsv &
  while (( $(jobs -r | wc -l) >= 6 )); do sleep 0.05; done
done
wait
