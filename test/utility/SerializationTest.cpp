#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

#include "HalStorageTestApi.h"
#include "Serialization.h"

namespace {

struct Pod {
  uint16_t a;
  uint32_t b;
  uint8_t c;
  bool operator==(const Pod& o) const = default;
};

template <typename T>
T roundTripPod(const T& v) {
  std::stringstream s;
  serialization::writePod(static_cast<std::ostream&>(s), v);
  T out{};
  serialization::readPod(static_cast<std::istream&>(s), out);
  return out;
}

std::string roundTripString(const std::string& s) {
  std::stringstream stream;
  serialization::writeString(static_cast<std::ostream&>(stream), s);
  std::string out;
  serialization::readString(static_cast<std::istream&>(stream), out);
  return out;
}

}  // namespace

// ---- writePod / readPod ------------------------------------------------------

TEST(SerializationPod, RoundTripsUint8) {
  EXPECT_EQ(roundTripPod<uint8_t>(0), 0u);
  EXPECT_EQ(roundTripPod<uint8_t>(0xFF), 0xFFu);
  EXPECT_EQ(roundTripPod<uint8_t>(0x5A), 0x5Au);
}

TEST(SerializationPod, RoundTripsUint16) {
  EXPECT_EQ(roundTripPod<uint16_t>(0xDEAD), 0xDEADu);
  EXPECT_EQ(roundTripPod<uint16_t>(0), 0u);
  EXPECT_EQ(roundTripPod<uint16_t>(0xFFFF), 0xFFFFu);
}

TEST(SerializationPod, RoundTripsUint32) {
  EXPECT_EQ(roundTripPod<uint32_t>(0xDEADBEEF), 0xDEADBEEFu);
  EXPECT_EQ(roundTripPod<uint32_t>(0), 0u);
  EXPECT_EQ(roundTripPod<uint32_t>(0xFFFFFFFF), 0xFFFFFFFFu);
}

TEST(SerializationPod, RoundTripsUint64) {
  EXPECT_EQ(roundTripPod<uint64_t>(0x0123456789ABCDEFull), 0x0123456789ABCDEFull);
}

TEST(SerializationPod, RoundTripsSignedTypes) {
  EXPECT_EQ(roundTripPod<int32_t>(-1), -1);
  EXPECT_EQ(roundTripPod<int32_t>(INT32_MIN), INT32_MIN);
  EXPECT_EQ(roundTripPod<int32_t>(INT32_MAX), INT32_MAX);
  EXPECT_EQ(roundTripPod<int64_t>(-1), -1);
  EXPECT_EQ(roundTripPod<int64_t>(INT64_MIN), INT64_MIN);
}

TEST(SerializationPod, RoundTripsFloat) {
  EXPECT_EQ(roundTripPod<float>(3.14159f), 3.14159f);
  EXPECT_EQ(roundTripPod<float>(0.0f), 0.0f);
  EXPECT_EQ(roundTripPod<float>(-1.5f), -1.5f);
}

TEST(SerializationPod, RoundTripsStruct) {
  Pod in{0x1234, 0xDEADBEEF, 0x42};
  Pod out = roundTripPod(in);
  EXPECT_EQ(out, in);
}

TEST(SerializationPod, WritesExactlySizeofBytes) {
  std::stringstream s;
  uint32_t v = 0xCAFEBABE;
  serialization::writePod(static_cast<std::ostream&>(s), v);
  EXPECT_EQ(s.str().size(), sizeof(uint32_t));
}

TEST(SerializationPod, SequentialWritesPreserveOrder) {
  std::stringstream s;
  serialization::writePod(static_cast<std::ostream&>(s), uint32_t{1});
  serialization::writePod(static_cast<std::ostream&>(s), uint32_t{2});
  serialization::writePod(static_cast<std::ostream&>(s), uint32_t{3});

  uint32_t a = 0, b = 0, c = 0;
  serialization::readPod(static_cast<std::istream&>(s), a);
  serialization::readPod(static_cast<std::istream&>(s), b);
  serialization::readPod(static_cast<std::istream&>(s), c);
  EXPECT_EQ(a, 1u);
  EXPECT_EQ(b, 2u);
  EXPECT_EQ(c, 3u);
}

// ---- writeString / readString ------------------------------------------------

TEST(SerializationString, RoundTripsAscii) {
  EXPECT_EQ(roundTripString("hello"), "hello");
  EXPECT_EQ(roundTripString("The quick brown fox"), "The quick brown fox");
}

TEST(SerializationString, RoundTripsEmpty) {
  EXPECT_EQ(roundTripString(""), "");
}

TEST(SerializationString, RoundTripsEmbeddedNulls) {
  std::string in;
  in.push_back('a');
  in.push_back('\0');
  in.push_back('b');
  in.push_back('\0');
  in.push_back('c');
  std::string out = roundTripString(in);
  ASSERT_EQ(out.size(), 5u);
  EXPECT_EQ(out, in);
}

TEST(SerializationString, RoundTripsUtf8Bytes) {
  // Raw byte preservation -- the format does not interpret encoding.
  std::string in = "caf\xC3\xA9 \xE2\x98\x83";  // "cafe + snowman" in UTF-8
  EXPECT_EQ(roundTripString(in), in);
}

TEST(SerializationString, LengthPrefixIsUint32) {
  std::stringstream s;
  serialization::writeString(static_cast<std::ostream&>(s), std::string("xy"));
  // 4 bytes prefix + 2 bytes payload.
  EXPECT_EQ(s.str().size(), sizeof(uint32_t) + 2);
}

TEST(SerializationString, EmptyStringStillWritesPrefix) {
  std::stringstream s;
  serialization::writeString(static_cast<std::ostream&>(s), std::string(""));
  EXPECT_EQ(s.str().size(), sizeof(uint32_t));
}

TEST(SerializationString, PrefixDecodesToActualLength) {
  std::stringstream s;
  std::string payload = "abcdef";
  serialization::writeString(static_cast<std::ostream&>(s), payload);
  uint32_t len = 0;
  serialization::readPod(static_cast<std::istream&>(s), len);
  EXPECT_EQ(len, payload.size());
}

// ---- HalFile read overloads --------------------------------------------------
//
// The stream and HalFile branches share format, so we encode via std::ostream,
// hand the bytes to the HalFile stub as readable content, and decode through
// the HalFile overload. A successful round-trip confirms both branches agree.

namespace {

std::string encodeViaStream(auto&& encode) {
  std::stringstream s;
  encode(static_cast<std::ostream&>(s));
  return s.str();
}

}  // namespace

TEST(SerializationHalFile, ReadPodFromSeededFile) {
  test_stubs::clearHalFileContent();
  std::string bytes = encodeViaStream(
      [](std::ostream& o) { serialization::writePod(o, uint32_t{0xCAFEBABE}); });
  HalFile file = test_stubs::makeReadableHalFile(std::move(bytes));
  uint32_t out = 0;
  serialization::readPod(file, out);
  EXPECT_EQ(out, 0xCAFEBABEu);
}

TEST(SerializationHalFile, ReadStringFromSeededFile) {
  test_stubs::clearHalFileContent();
  std::string bytes = encodeViaStream([](std::ostream& o) {
    serialization::writeString(o, std::string("greetings"));
  });
  HalFile file = test_stubs::makeReadableHalFile(std::move(bytes));
  std::string out;
  serialization::readString(file, out);
  EXPECT_EQ(out, "greetings");
}

TEST(SerializationHalFile, ReadMixedSequenceFromSeededFile) {
  test_stubs::clearHalFileContent();
  std::string bytes = encodeViaStream([](std::ostream& o) {
    serialization::writePod(o, uint32_t{0xAA});
    serialization::writeString(o, std::string("payload"));
    serialization::writePod(o, uint16_t{0xBB});
  });
  HalFile file = test_stubs::makeReadableHalFile(std::move(bytes));
  uint32_t a = 0;
  std::string mid;
  uint16_t b = 0;
  serialization::readPod(file, a);
  serialization::readString(file, mid);
  serialization::readPod(file, b);
  EXPECT_EQ(a, 0xAAu);
  EXPECT_EQ(mid, "payload");
  EXPECT_EQ(b, 0xBBu);
}

// ---- mixed pod + string ------------------------------------------------------

TEST(SerializationMixed, PodStringPodSequence) {
  std::stringstream s;
  serialization::writePod(static_cast<std::ostream&>(s), uint32_t{0xAA});
  serialization::writeString(static_cast<std::ostream&>(s), std::string("middle"));
  serialization::writePod(static_cast<std::ostream&>(s), uint16_t{0xBB});

  uint32_t a = 0;
  std::string mid;
  uint16_t b = 0;
  serialization::readPod(static_cast<std::istream&>(s), a);
  serialization::readString(static_cast<std::istream&>(s), mid);
  serialization::readPod(static_cast<std::istream&>(s), b);
  EXPECT_EQ(a, 0xAAu);
  EXPECT_EQ(mid, "middle");
  EXPECT_EQ(b, 0xBBu);
}
