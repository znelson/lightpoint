#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "lib/JsonIO/JsonWriter.h"
#include "lib/JsonIO/StreamingJsonParser.h"

namespace {

// Build a JSON string starting from an empty std::string, returning the
// caller-owned buffer for direct comparison.
std::string emit(void (*build)(JsonWriter&)) {
  std::string out;
  JsonWriter w(out);
  build(w);
  return out;
}

}  // namespace

// ---- Structural --------------------------------------------------------------

TEST(JsonWriter, EmptyObject) {
  std::string out;
  JsonWriter w(out);
  w.beginObject();
  w.endObject();
  EXPECT_EQ(out, "{}");
}

TEST(JsonWriter, EmptyArray) {
  std::string out;
  JsonWriter w(out);
  w.beginArray();
  w.endArray();
  EXPECT_EQ(out, "[]");
}

TEST(JsonWriter, SingleStringMember) {
  std::string out;
  JsonWriter w(out);
  w.beginObject();
  w.key("k");
  w.valueString("v");
  w.endObject();
  EXPECT_EQ(out, R"({"k":"v"})");
}

TEST(JsonWriter, MultipleMembers) {
  std::string out;
  JsonWriter w(out);
  w.beginObject();
  w.key("a");
  w.valueInt(1);
  w.key("b");
  w.valueInt(2);
  w.key("c");
  w.valueInt(3);
  w.endObject();
  EXPECT_EQ(out, R"({"a":1,"b":2,"c":3})");
}

TEST(JsonWriter, ArrayOfNumbers) {
  std::string out;
  JsonWriter w(out);
  w.beginArray();
  w.valueInt(1);
  w.valueInt(2);
  w.valueInt(3);
  w.endArray();
  EXPECT_EQ(out, "[1,2,3]");
}

TEST(JsonWriter, ArrayOfStrings) {
  std::string out;
  JsonWriter w(out);
  w.beginArray();
  w.valueString("a");
  w.valueString("b");
  w.endArray();
  EXPECT_EQ(out, R"(["a","b"])");
}

TEST(JsonWriter, NestedObjectInArray) {
  std::string out;
  JsonWriter w(out);
  w.beginArray();
  w.beginObject();
  w.key("k");
  w.valueString("v");
  w.endObject();
  w.endArray();
  EXPECT_EQ(out, R"([{"k":"v"}])");
}

TEST(JsonWriter, NestedArrayInObject) {
  std::string out;
  JsonWriter w(out);
  w.beginObject();
  w.key("items");
  w.beginArray();
  w.valueInt(10);
  w.valueInt(20);
  w.endArray();
  w.endObject();
  EXPECT_EQ(out, R"({"items":[10,20]})");
}

TEST(JsonWriter, MultipleObjectsInArray) {
  std::string out;
  JsonWriter w(out);
  w.beginArray();
  for (int i = 0; i < 3; ++i) {
    w.beginObject();
    w.key("n");
    w.valueInt(i);
    w.endObject();
  }
  w.endArray();
  EXPECT_EQ(out, R"([{"n":0},{"n":1},{"n":2}])");
}

// ---- Primitive value formatting ---------------------------------------------

TEST(JsonWriter, BoolValues) {
  std::string out;
  JsonWriter w(out);
  w.beginObject();
  w.key("t");
  w.valueBool(true);
  w.key("f");
  w.valueBool(false);
  w.endObject();
  EXPECT_EQ(out, R"({"t":true,"f":false})");
}

TEST(JsonWriter, IntValues) {
  std::string out;
  JsonWriter w(out);
  w.beginArray();
  w.valueInt(0);
  w.valueInt(-1);
  w.valueInt(2147483647);
  w.valueInt(-2147483648LL);
  w.endArray();
  EXPECT_EQ(out, "[0,-1,2147483647,-2147483648]");
}

TEST(JsonWriter, UIntValues) {
  std::string out;
  JsonWriter w(out);
  w.beginArray();
  w.valueUInt(0);
  w.valueUInt(255);
  w.valueUInt(4294967295u);
  w.endArray();
  EXPECT_EQ(out, "[0,255,4294967295]");
}

TEST(JsonWriter, NullValue) {
  std::string out;
  JsonWriter w(out);
  w.beginObject();
  w.key("x");
  w.valueNull();
  w.endObject();
  EXPECT_EQ(out, R"({"x":null})");
}

// ---- String escaping ---------------------------------------------------------

TEST(JsonWriter, EscapesQuoteAndBackslash) {
  std::string out;
  JsonWriter w(out);
  w.beginObject();
  w.key("k");
  w.valueString(std::string("a\"b\\c"));
  w.endObject();
  // The literal value contains a quote and a backslash; both must be escaped.
  EXPECT_EQ(out, R"({"k":"a\"b\\c"})");
}

TEST(JsonWriter, EscapesControlChars) {
  std::string out;
  JsonWriter w(out);
  w.beginArray();
  w.valueString(std::string("\b"));
  w.valueString(std::string("\f"));
  w.valueString(std::string("\n"));
  w.valueString(std::string("\r"));
  w.valueString(std::string("\t"));
  w.endArray();
  EXPECT_EQ(out, R"(["\b","\f","\n","\r","\t"])");
}

TEST(JsonWriter, EscapesOtherC0AsUnicodeEscape) {
  // Control bytes 0x00-0x1F that don't have a short escape must use \u00XX.
  std::string out;
  JsonWriter w(out);
  w.beginArray();
  w.valueString(std::string(1, '\x01'));
  w.valueString(std::string(1, '\x1F'));
  w.endArray();
  EXPECT_EQ(out, "[\"\\u0001\",\"\\u001F\"]");
}

TEST(JsonWriter, PassesThroughRawUtf8) {
  // High-bit bytes (UTF-8 continuation / lead) must pass through unescaped.
  // Input: "café" -> 63 61 66 C3 A9
  std::string out;
  JsonWriter w(out);
  w.beginObject();
  w.key("k");
  w.valueString(std::string("caf\xC3\xA9"));
  w.endObject();
  EXPECT_EQ(out, std::string("{\"k\":\"caf\xC3\xA9\"}"));
}

TEST(JsonWriter, EscapesInsideKeys) {
  std::string out;
  JsonWriter w(out);
  w.beginObject();
  w.key("a\"b");
  w.valueInt(1);
  w.endObject();
  EXPECT_EQ(out, R"({"a\"b":1})");
}

TEST(JsonWriter, EmptyStringValue) {
  std::string out;
  JsonWriter w(out);
  w.beginObject();
  w.key("k");
  w.valueString("");
  w.endObject();
  EXPECT_EQ(out, R"({"k":""})");
}

// ---- Round trip with StreamingJsonParser -------------------------------------

namespace {

enum class EventType {
  KEY,
  STRING,
  NUMBER,
  BOOL_TRUE,
  BOOL_FALSE,
  NULL_VAL,
  OBJECT_START,
  OBJECT_END,
  ARRAY_START,
  ARRAY_END,
};
struct Event {
  EventType type;
  std::string value;
};
struct Ctx {
  std::vector<Event> events;
};

JsonCallbacks makeParseCallbacks(Ctx* c) {
  return {
      c,
      [](void* p, const char* k, size_t n) {
        static_cast<Ctx*>(p)->events.push_back({EventType::KEY, std::string(k, n)});
      },
      [](void* p, const char* v, size_t n) {
        static_cast<Ctx*>(p)->events.push_back({EventType::STRING, std::string(v, n)});
      },
      [](void* p, const char* v, size_t n) {
        static_cast<Ctx*>(p)->events.push_back({EventType::NUMBER, std::string(v, n)});
      },
      [](void* p, bool v) {
        static_cast<Ctx*>(p)->events.push_back({v ? EventType::BOOL_TRUE : EventType::BOOL_FALSE, {}});
      },
      [](void* p) { static_cast<Ctx*>(p)->events.push_back({EventType::NULL_VAL, {}}); },
      [](void* p) { static_cast<Ctx*>(p)->events.push_back({EventType::OBJECT_START, {}}); },
      [](void* p) { static_cast<Ctx*>(p)->events.push_back({EventType::OBJECT_END, {}}); },
      [](void* p) { static_cast<Ctx*>(p)->events.push_back({EventType::ARRAY_START, {}}); },
      [](void* p) { static_cast<Ctx*>(p)->events.push_back({EventType::ARRAY_END, {}}); },
  };
}

}  // namespace

TEST(JsonWriter, OutputParsesBackToSameEvents) {
  // Build a non-trivial document, then feed it through StreamingJsonParser
  // and check that the event stream matches what we emitted.
  std::string out;
  JsonWriter w(out);
  w.beginObject();
  w.key("name");
  w.valueString(std::string("ca\xC3\xA9"));  // café-style UTF-8
  w.key("age");
  w.valueInt(42);
  w.key("active");
  w.valueBool(true);
  w.key("nothing");
  w.valueNull();
  w.key("tags");
  w.beginArray();
  w.valueString("x");
  w.valueString("y");
  w.endArray();
  w.key("nested");
  w.beginObject();
  w.key("k");
  w.valueString(std::string("a\"b"));
  w.endObject();
  w.endObject();

  Ctx ctx;
  StreamingJsonParser parser(makeParseCallbacks(&ctx));
  parser.feed(out.data(), out.size());
  EXPECT_FALSE(parser.hasError());

  // Spot-check key milestones; gtest will print the whole event log if any fail.
  ASSERT_GE(ctx.events.size(), 1u);
  EXPECT_EQ(ctx.events.front().type, EventType::OBJECT_START);
  EXPECT_EQ(ctx.events.back().type, EventType::OBJECT_END);

  // The string with the embedded escaped quote must round-trip to its
  // original literal value (a"b -- 3 chars, including the quote).
  bool foundEscapedQuoteValue = false;
  for (auto& e : ctx.events) {
    if (e.type == EventType::STRING && e.value == "a\"b") foundEscapedQuoteValue = true;
  }
  EXPECT_TRUE(foundEscapedQuoteValue);

  // The UTF-8 string value must round-trip byte-for-byte.
  bool foundUtf8Value = false;
  for (auto& e : ctx.events) {
    if (e.type == EventType::STRING && e.value == std::string("ca\xC3\xA9")) foundUtf8Value = true;
  }
  EXPECT_TRUE(foundUtf8Value);
}

TEST(JsonWriter, BuiltExamplesMatchExpectedShapes) {
  // A few small whole-document expectations -- catches stray commas or
  // colons better than the streaming-roundtrip test, which is tolerant of
  // whitespace differences.
  EXPECT_EQ(emit([](JsonWriter& w) {
              w.beginObject();
              w.endObject();
            }),
            "{}");
  EXPECT_EQ(emit([](JsonWriter& w) {
              w.beginArray();
              w.endArray();
            }),
            "[]");
  EXPECT_EQ(emit([](JsonWriter& w) {
              w.beginObject();
              w.key("a");
              w.beginArray();
              w.endArray();
              w.endObject();
            }),
            R"({"a":[]})");
  EXPECT_EQ(emit([](JsonWriter& w) {
              w.beginArray();
              w.beginArray();
              w.endArray();
              w.beginArray();
              w.endArray();
              w.endArray();
            }),
            "[[],[]]");
}
