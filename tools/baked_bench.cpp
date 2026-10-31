// Measures src/native/baked_buffer on the real command lists a census saved: what decoding costs once, and what
// replaying a frame costs, against the 7 microsecond per-draw budget in docs/native-rendering-survey.md.
//
// Build: clang++ -std=c++20 -O2 -Isrc/native tools/baked_bench.cpp src/native/baked_buffer.cpp -o baked_bench
// Run:   ./baked_bench <census-label.lists> <frames in the census>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>

#include "baked_buffer.h"

using namespace crackdown::native;

namespace {

double NowUs() {
  using namespace std::chrono;
  return duration<double, std::micro>(steady_clock::now().time_since_epoch()).count();
}

struct Saved {
  uint32_t buffer;
  uint32_t runs;
  std::vector<uint32_t> words;
  BakedList decoded;
};

// What a real replay touches per draw: the literals, which need a byte swap, plus redundant-state checks.
struct Sink {
  uint32_t pipeline = ~0u, textures = ~0u, dynamic = ~0u;
  uint64_t binds = 0, draws = 0, constant_dwords = 0;
  uint32_t constants[8192] = {};
  uint32_t literals[64] = {};
  const uint32_t* guest = nullptr;
  uint32_t guest_words = 0;

  void Draw(const BakedList& list, const BakedDraw& draw) {
    for (uint32_t i = 0; i < draw.constant_writes; ++i) {
      const BakedConstantWrite& write = list.constant_writes[draw.first_constant_write + i];
      std::memcpy(constants + write.first_dword, list.constant_data.data() + write.data, write.dwords * sizeof(uint32_t));
      constant_dwords += write.dwords;
    }
    binds += draw.pipeline != pipeline;
    binds += draw.textures != textures;
    binds += draw.dynamic != dynamic;
    pipeline = draw.pipeline;
    textures = draw.textures;
    dynamic = draw.dynamic;
    const uint32_t dwords = std::min<uint32_t>(draw.literal_dwords, 64);
    const uint32_t* source = guest + (draw.literals >> 2) % (guest_words - 64);
    for (uint32_t i = 0; i < dwords; ++i) {
      literals[i] = __builtin_bswap32(source[i]);
    }
    ++draws;
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: baked_bench <lists> <frames>\n");
    return 2;
  }
  const int frames_in_census = atoi(argv[2]);
  std::ifstream input(argv[1], std::ios::binary);
  std::vector<char> file((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

  std::vector<Saved> saved;
  size_t offset = 0;
  while (offset + 24 <= file.size()) {
    uint32_t record[6];
    std::memcpy(record, file.data() + offset, 24);
    offset += 24;
    Saved s{record[0], record[3], {}, {}};
    s.words.resize(record[5]);
    std::memcpy(s.words.data(), file.data() + offset, size_t(record[5]) * 4);
    offset += size_t(record[5]) * 4;
    for (uint32_t& word : s.words) {
      word = __builtin_bswap32(word);
    }
    saved.push_back(std::move(s));
  }

  BakedLibrary library;
  double decode_start = NowUs();
  for (Saved& s : saved) {
    s.decoded = library.Decode(s.words.data(), s.words.size());
  }
  const double decode_us = NowUs() - decode_start;

  uint64_t decoded_draws = 0, weighted_draws = 0;
  uint32_t most = 0;
  for (const Saved& s : saved) {
    decoded_draws += s.decoded.draws.size();
    weighted_draws += s.decoded.draws.size() * uint64_t(s.runs);
    most = std::max<uint32_t>(most, uint32_t(s.decoded.draws.size()));
  }
  printf("lists %zu, draws in them %llu, largest list %u draws\n", saved.size(), (unsigned long long)decoded_draws, most);
  printf("draws per frame %.0f (census-lists.py reports the same total)\n", double(weighted_draws) / frames_in_census);
  printf("unique pipelines %zu, texture sets %zu, dynamic states %zu\n", library.pipeline_count(),
         library.texture_set_count(), library.dynamic_state_count());
  printf("packets %llu, unknown %llu, truncated %llu\n", (unsigned long long)library.stats().packets,
         (unsigned long long)library.stats().unknown_packets, (unsigned long long)library.stats().truncated_packets);
  printf("decode once: %.0f us total, %.1f us per list, %.2f us per draw\n", decode_us, decode_us / saved.size(),
         decode_us / decoded_draws);

  // One census frame's worth of runs, shuffled, because the game doesn't run lists in file order.
  std::vector<const BakedList*> frame;
  for (const Saved& s : saved) {
    const uint32_t per_frame = (s.runs + frames_in_census / 2) / frames_in_census;
    for (uint32_t i = 0; i < std::max<uint32_t>(per_frame, 1); ++i) {
      frame.push_back(&s.decoded);
    }
  }
  std::mt19937 random(1234);
  std::shuffle(frame.begin(), frame.end(), random);

  std::vector<uint32_t> guest(1 << 20);
  for (uint32_t i = 0; i < guest.size(); ++i) {
    guest[i] = i * 2654435761u;
  }

  std::vector<double> times;
  static Sink sink;
  sink.guest = guest.data();
  sink.guest_words = uint32_t(guest.size());
  for (int repeat = 0; repeat < 300; ++repeat) {
    sink.draws = 0;
    sink.binds = 0;
    sink.constant_dwords = 0;
    const double start = NowUs();
    for (const BakedList* list : frame) {
      for (const BakedDraw& draw : list->draws) {
        if (DrawPasses(draw, 1)) {
          sink.Draw(*list, draw);
        }
      }
    }
    times.push_back(NowUs() - start);
  }
  std::sort(times.begin(), times.end());
  const double median = times[times.size() / 2];
  printf("replay: %zu runs, %llu draws, %llu state changes, %.1f constant dwords per draw, median %.1f us, %.3f us per draw, worst %.1f us\n",
         frame.size(), (unsigned long long)sink.draws, (unsigned long long)sink.binds,
         double(sink.constant_dwords) / sink.draws, median, median / sink.draws, times.back());
  return 0;
}
