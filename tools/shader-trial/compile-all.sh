#!/bin/zsh
# Second half of the trial: compile the translated shaders with Apple's compiler. This toolchain no
# longer predefines __air__, which the generated code branches on, so it's passed here.
cd "${0:A:h}"
print -rl -- translated/*.metal | xargs -P 6 -n 1 zsh -c '
  n=$(basename $1 .metal); o=translated/$n
  if xcrun -sdk macosx metal -D__air__=1 -c $1 -o $o.air 2> $o.compile.txt; then
    printf "%s\tok\n" $n
  else
    printf "%s\tcompile-failed\t%s\n" $n "$(grep -m1 error: $o.compile.txt | sed -E "s/^[^ ]+ error: //" | cut -c1-140)"
  fi' zsh > compiled.tsv
