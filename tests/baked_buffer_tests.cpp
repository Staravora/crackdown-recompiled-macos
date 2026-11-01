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

void TestStateCarriesFromOneDrawToTheNext() {
  BakedLibrary library;
  std::vector<uint32_t> first;
  AddVertexShader(first, 0x1000, 90);
  first.insert(first.end(), {RegisterWrite(kViewport, 1), 0x44200000});
  first.insert(first.end(), {RegisterWrite(kFetchConstants + 6, 1), 0x12345678});
  first.insert(first.end(), {RegisterWrite(kFloatConstants + 4, 1), 0x3F800000});
  AddDraw(first, 0x2000);
  BakedList list;
  CHECK(library.DecodeNext(first.data(), first.size(), list));
  CHECK(library.vertex_shader_loaded());

  std::vector<uint32_t> second;
  AddDraw(second, 0x3000);
  BakedList next;
  CHECK(library.DecodeNext(second.data(), second.size(), next));
  CHECK(!library.vertex_shader_loaded());
  CHECK(next.draws.size() == 1);
  CHECK(next.draws[0].index_buffer == 0x3000);
  CHECK(next.draws[0].vertex_shader == 0x1000);
  CHECK(next.draws[0].viewport[0] == 0x44200000);
  CHECK(next.fetch_data.size() == 192);
  CHECK(next.fetch_data[6] == 0x12345678);
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

void TestOnlyTheLastDrawOfACallIsKept() {
  BakedLibrary library;
  std::vector<uint32_t> words;
  AddDraw(words, 0x2000);
  AddDraw(words, 0x3000);
  BakedList list;
  CHECK(library.DecodeNext(words.data(), words.size(), list));
  CHECK(list.draws.size() == 1);
  CHECK(list.draws[0].index_buffer == 0x3000);
  CHECK(list.draws[0].fetch_constants == 0);
}

void TestWordsWithNoDrawGiveNoDraw() {
  BakedLibrary library;
  const std::vector<uint32_t> words = {RegisterWrite(kViewport, 1), 0x44200000};
  BakedList list;
  CHECK(!library.DecodeNext(words.data(), words.size(), list));
}

void TestVerticesInTheRingStopTheRead() {
  BakedLibrary library;
  std::vector<uint32_t> first;
  first.insert(first.end(), {RegisterWrite(kViewport, 1), 0x44200000});
  AddDraw(first, 0x2000);
  BakedList list;
  CHECK(library.DecodeNext(first.data(), first.size(), list));

  // A float that reads as packet type 1, then one that would be a register write if the read went on.
  std::vector<uint32_t> second = {0x4443D29C, 0, 0, RegisterWrite(kViewport, 1), 0x11111111};
  AddDraw(second, 0x3000);
  BakedList next;
  CHECK(!library.DecodeNext(second.data(), second.size(), next));

  std::vector<uint32_t> third;
  AddDraw(third, 0x4000);
  CHECK(library.DecodeNext(third.data(), third.size(), next));
  CHECK(next.draws[0].viewport[0] == 0x44200000);
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
  std::vector<uint32_t> first;
  AddLiterals(first, 0x5000, 0x7F0);
  AddLiterals(first, 0x6000, 0x3F0);
  AddVertexShader(first, 0x1000, 90);
  AddDraw(first, 0x2000);
  BakedList list;
  CHECK(library.DecodeNext(first.data(), first.size(), list));

  std::vector<uint32_t> second;
  AddLiterals(second, 0x7000, 0x3F0);
  AddVertexShader(second, 0x1100, 90);
  AddDraw(second, 0x3000);
  BakedList next;
  CHECK(library.DecodeNext(second.data(), second.size(), next));
  CHECK(next.draws[0].literal_count == 2);
  CHECK(next.draws[0].literals[0].address == 0x5000);
  CHECK(next.draws[0].literals[1].address == 0x7000);
}

void TestAnImpossibleShaderSizeIsIgnored() {
  BakedLibrary library;
  std::vector<uint32_t> words;
  AddVertexShader(words, 0x1000, 90);
  AddVertexShader(words, 0x9000, 4096 * 3 + 1);
  AddDraw(words, 0x2000);
  CHECK(library.Decode(words.data(), words.size()).draws[0].vertex_shader == 0x1000);
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
  TestOnlyTheLastDrawOfACallIsKept();
  TestWordsWithNoDrawGiveNoDraw();
  TestVerticesInTheRingStopTheRead();
  TestAShaderKeepsEveryBlockOfLiterals();
  TestANewShaderReplacesOnlyItsOwnStagesLiterals();
  TestAnImpossibleShaderSizeIsIgnored();
  TestADrawWithoutAnIndexBufferReadsNoFurther();
  return check::Finish("baked_buffer_tests");
}
