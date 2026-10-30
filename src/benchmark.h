#pragma once

#include <cstdint>
#include <filesystem>

namespace rex::input {
class InputSystem;
}

namespace crackdown::benchmark {

// With crackdown_benchmark on, a virtual pad presses through the title and main menu to continue the saved game and
// spawns at the first supply point of crackdown_benchmark_district, then stands still: after a warm-up, the guest
// frame times of a fixed window are written as JSON to `output_dir` and the process exits. Exit code 0 means a result
// was written, 2 that gameplay never started (relaunch).
void Install(rex::input::InputSystem* input, uint8_t* membase, const std::filesystem::path& output_dir);

// Called for every new guest frame (not repeated presents).
void OnGuestFrame();

}  // namespace crackdown::benchmark
