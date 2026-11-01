#include <cstdint>
#include <vector>

#include "check.h"
#include "native/baked_buffer.h"

using namespace crackdown::native;

namespace {

constexpr uint32_t kViewport = 0x210F;
constexpr uint32_t kFloatConstants = 0x4000;
constexpr uint32_t kFetchConstants = 0x4800;
constexpr uint32_t kStripOfSixteen = 0x00100006;

// Stands in for guest physical memory.
const uint8_t g_memory[0x10000] = {};

uint32_t RegisterWrite(uint32_t address, uint32_t dwords) {
  return ((dwords - 1) << 16) | address;
}

uint32_t Packet(uint32_t opcode, uint32_t dwords) {
  return 0xC0000000 | ((dwords - 1) << 16) | (opcode << 8);
}

void AddDraw(std::vector<uint32_t>& words, uint32_t index_buffer) {
  words.insert(words.end(), {Packet(0x22, 4), 0, kStripOfSixteen, index_buffer, 0x40000010});
}

void AddVertexShader(std::vector<uint32_t>& words, uint32_t address, uint32_t dwords) {
  words.insert(words.end(), {Packet(0x27, 2), address, dwords});
}

void AddLiterals(std::vector<uint32_t>& words, uint32_t address, uint32_t first_dword) {
  words.insert(words.end(), {Packet(0x2F, 3), address, first_dword, 16});
}

struct RingRead {
  std::vector<BakedDraw> draws;
  std::vector<uint32_t> fetch;
  size_t used = 0;
};

RingRead ReadRing(BakedLibrary& library, const std::vector<uint32_t>& words, const uint8_t* memory = g_memory) {
  RingRead read;
  read.used = library.DecodeRing(words.data(), words.size(), memory, [&](const BakedDraw& draw, const uint32_t* fetch) {
    read.draws.push_back(draw);
    read.fetch.assign(fetch, fetch + 192);
  });
  return read;
}

void TestStateCarriesFromOneDrawToTheNext() {
  BakedLibrary library;
  std::vector<uint32_t> first;
  AddVertexShader(first, 0x1000, 90);
  first.insert(first.end(), {RegisterWrite(kViewport, 1), 0x44200000});
  first.insert(first.end(), {RegisterWrite(kFetchConstants + 6, 1), 0x12345678});
  first.insert(first.end(), {RegisterWrite(kFloatConstants + 4, 1), 0x3F800000});
  AddDraw(first, 0x2000);
  CHECK(ReadRing(library, first).draws.size() == 1);
  CHECK(library.TakeVertexShaderLoaded());

  std::vector<uint32_t> second;
  AddDraw(second, 0x3000);
  const RingRead next = ReadRing(library, second);
  CHECK(!library.TakeVertexShaderLoaded());
  CHECK(next.draws.size() == 1);
  CHECK(next.draws[0].index_buffer == 0x3000);
  CHECK(next.draws[0].vertex_shader == 0x1000);
  CHECK(next.draws[0].viewport[0] == 0x44200000);
  CHECK(next.fetch[6] == 0x12345678);
  CHECK(library.vertex_constants()[4] == 0x3F800000);
}

void TestAListStartsFromNothing() {
  BakedLibrary library;
  std::vector<uint32_t> first;
  AddVertexShader(first, 0x1000, 90);
  AddDraw(first, 0x2000);
  library.Decode(first.data(), first.size());
  std::vector<uint32_t> second;
  AddDraw(second, 0x3000);
  CHECK(library.Decode(second.data(), second.size()).draws[0].vertex_shader == 0);
}

void TestEveryDrawInAStretchIsSeenInOrder() {
  BakedLibrary library;
  std::vector<uint32_t> words;
  AddDraw(words, 0x2000);
  words.insert(words.end(), {RegisterWrite(kViewport, 1), 0x44200000});
  AddDraw(words, 0x3000);
  const RingRead read = ReadRing(library, words);
  CHECK(read.draws.size() == 2);
  CHECK(read.draws[0].index_buffer == 0x2000);
  CHECK(read.draws[0].viewport[0] == 0);
  CHECK(read.draws[1].index_buffer == 0x3000);
  CHECK(read.draws[1].viewport[0] == 0x44200000);
  CHECK(read.used == words.size());
}

void TestAPacketCutOffIsLeftForNextTime() {
  BakedLibrary library;
  std::vector<uint32_t> words;
  AddDraw(words, 0x2000);
  const size_t whole = words.size();
  AddDraw(words, 0x3000);
  words.resize(words.size() - 2);
  const RingRead read = ReadRing(library, words);
  CHECK(read.draws.size() == 1);
  CHECK(read.used == whole);
}

void TestAMisreadRingStopsWithoutWritingRegisters() {
  BakedLibrary library;
  std::vector<uint32_t> first;
  first.insert(first.end(), {RegisterWrite(kViewport, 1), 0x44200000});
  AddDraw(first, 0x2000);
  ReadRing(library, first);

  // A float that reads as packet type 1, then one that would be a register write if the read went on.
  std::vector<uint32_t> second = {0x4443D29C, 0, 0, RegisterWrite(kViewport, 1), 0x11111111};
  AddDraw(second, 0x3000);
  const RingRead stopped = ReadRing(library, second);
  CHECK(stopped.draws.empty());
  CHECK(stopped.used == second.size());
  CHECK(library.stats().ring_desyncs == 1);

  std::vector<uint32_t> third;
  AddDraw(third, 0x4000);
  CHECK(ReadRing(library, third).draws[0].viewport[0] == 0x44200000);
}

void TestAShaderKeepsEveryBlockOfLiterals() {
  BakedLibrary library;
  std::vector<uint32_t> words;
  AddLiterals(words, 0x5000, 0x7F0);
  AddLiterals(words, 0x6000, 0x3B0);
  AddLiterals(words, 0x6040, 0x3F0);
  AddVertexShader(words, 0x1000, 90);
  AddDraw(words, 0x2000);
  const BakedList list = library.Decode(words.data(), words.size());
  CHECK(list.draws[0].literal_count == 3);
  CHECK(list.draws[0].literals[1].address == 0x6000);
  CHECK(list.draws[0].literals[1].first_dword == 0x3B0);
  CHECK(list.draws[0].literals[2].first_dword == 0x3F0);
}

void TestANewShaderReplacesOnlyItsOwnStagesLiterals() {
  BakedLibrary library;
  std::vector<uint32_t> words;
  AddLiterals(words, 0x5000, 0x7F0);
  AddLiterals(words, 0x6000, 0x3F0);
  AddVertexShader(words, 0x1000, 90);
  AddDraw(words, 0x2000);
  AddLiterals(words, 0x7000, 0x3F0);
  AddVertexShader(words, 0x1100, 90);
  AddDraw(words, 0x3000);
  const BakedList list = library.Decode(words.data(), words.size());
  CHECK(list.draws[1].literal_count == 2);
  CHECK(list.draws[1].literals[0].address == 0x5000);
  CHECK(list.draws[1].literals[1].address == 0x7000);
}

void TestTheRingLoadsConstantsFromMemoryAsTheGpuWould() {
  uint8_t memory[0x10000] = {};
  memory[0x5000] = 0x3F;
  memory[0x5001] = 0x80;
  BakedLibrary library;
  std::vector<uint32_t> words;
  AddLiterals(words, 0x5000, 0x10);
  AddDraw(words, 0x2000);
  const RingRead read = ReadRing(library, words, memory);
  CHECK(library.vertex_constants()[0x10] == 0x3F800000);
  CHECK(read.draws[0].literal_count == 0);
}

void TestTheRingSetsRegistersWithSetConstant() {
  BakedLibrary library;
  std::vector<uint32_t> words = {Packet(0x2D, 3), (4u << 16) | (kViewport - 0x2000), 0x44200000, 0x44000000};
  AddDraw(words, 0x2000);
  const RingRead read = ReadRing(library, words);
  CHECK(read.draws[0].viewport[0] == 0x44200000);
  CHECK(read.draws[0].viewport[1] == 0x44000000);
}

void TestAnImpossibleShaderSizeIsIgnored() {
  BakedLibrary library;
  std::vector<uint32_t> words;
  AddVertexShader(words, 0x1000, 90);
  AddVertexShader(words, 0x9000, 4096 * 3 + 1);
  AddDraw(words, 0x2000);
  CHECK(library.Decode(words.data(), words.size()).draws[0].vertex_shader == 0x1000);
}

void TestAVertexShaderInsideTheRingComesWithItsCode() {
  BakedLibrary library;
  std::vector<uint32_t> words;
  AddVertexShader(words, 0x1000, 90);
  words.insert(words.end(), {Packet(0x2B, 5), 0, 3, 0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC});
  AddDraw(words, 0x2000);
  const RingRead read = ReadRing(library, words);
  CHECK(read.draws[0].vertex_shader == 0);
  CHECK(read.draws[0].vertex_shader_dwords == 3);
  CHECK(library.TakeVertexShaderLoaded());
  CHECK(library.inline_vertex_shader().size() == 3);
  CHECK(library.inline_vertex_shader()[2] == 0xCCCCCCCC);

  std::vector<uint32_t> next;
  AddVertexShader(next, 0x1100, 90);
  AddDraw(next, 0x3000);
  CHECK(ReadRing(library, next).draws[0].vertex_shader == 0x1100);
  CHECK(library.inline_vertex_shader().empty());
}

void TestAPixelShaderInsideTheRingLeavesTheVertexShaderAlone() {
  BakedLibrary library;
  std::vector<uint32_t> words;
  AddVertexShader(words, 0x1000, 90);
  words.insert(words.end(), {Packet(0x2B, 3), 1, 1, 0xAAAAAAAA});
  AddDraw(words, 0x2000);
  CHECK(ReadRing(library, words).draws[0].vertex_shader == 0x1000);
}

void TestADrawWithoutAnIndexBufferReadsNoFurther() {
  BakedLibrary library;
  const std::vector<uint32_t> words = {Packet(0x22, 2), 0, 0x00040086, Packet(0x2F, 3), 0xAAAA, 0, 16};
  const BakedList list = library.Decode(words.data(), words.size());
  CHECK(list.draws.size() == 1);
  CHECK(list.draws[0].index_buffer == 0);
  CHECK(list.draws[0].index_info == 0);
}

}  // namespace

int main() {
  TestStateCarriesFromOneDrawToTheNext();
  TestAListStartsFromNothing();
  TestEveryDrawInAStretchIsSeenInOrder();
  TestAPacketCutOffIsLeftForNextTime();
  TestAMisreadRingStopsWithoutWritingRegisters();
  TestAShaderKeepsEveryBlockOfLiterals();
  TestANewShaderReplacesOnlyItsOwnStagesLiterals();
  TestTheRingLoadsConstantsFromMemoryAsTheGpuWould();
  TestTheRingSetsRegistersWithSetConstant();
  TestAnImpossibleShaderSizeIsIgnored();
  TestAVertexShaderInsideTheRingComesWithItsCode();
  TestAPixelShaderInsideTheRingLeavesTheVertexShaderAlone();
  TestADrawWithoutAnIndexBufferReadsNoFurther();
  return check::Finish("baked_buffer_tests");
}
