#include <filesystem>
#include <string>
#include <vector>

#include "check.h"
#include "updater/update_platform.h"

int main(int argc, char** argv) {
  const auto executable = trg::update::CurrentExecutablePath();
  CHECK(executable.is_absolute());
  CHECK(std::filesystem::equivalent(executable, argv[0]));
  std::vector<std::string> expected;
  for (int i = 1; i < argc; ++i) {
    expected.emplace_back(argv[i]);
  }
  CHECK(trg::update::CurrentProcessArguments() == expected);
  return check::Finish("macOS executable and process arguments");
}
