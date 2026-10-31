// Runs XenosRecomp's translator on one extracted shader. One process per shader, so a crash in the
// translator counts as that shader's failure and not the whole run's.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "shader_recompiler.h"

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: xenos_one <shader.bin> <out.metal> <shader_common.h>\n");
    return 2;
  }
  std::ifstream input(argv[1], std::ios::binary);
  std::vector<char> data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  std::ifstream common(argv[3], std::ios::binary);
  std::string include((std::istreambuf_iterator<char>(common)), std::istreambuf_iterator<char>());
  ShaderRecompiler recompiler;
  recompiler.recompile(reinterpret_cast<const uint8_t*>(data.data()), include);
  std::ofstream output(argv[2], std::ios::binary);
  output.write(recompiler.out.data(), recompiler.out.size());
  return 0;
}
