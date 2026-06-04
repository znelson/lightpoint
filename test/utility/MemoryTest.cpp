#include <gtest/gtest.h>

#include <memory>
#include <type_traits>
#include <utility>

#include "Memory.h"

namespace {

struct Counted {
  static int liveCount;
  int value;
  Counted() : value(0) { ++liveCount; }
  explicit Counted(int v) : value(v) { ++liveCount; }
  Counted(int a, int b) : value(a + b) { ++liveCount; }
  Counted(const Counted& other) : value(other.value) { ++liveCount; }
  Counted(Counted&& other) noexcept : value(other.value) { ++liveCount; }
  ~Counted() { --liveCount; }
};
int Counted::liveCount = 0;

struct MoveOnlyArg {
  int v;
  MoveOnlyArg(int x) : v(x) {}
  MoveOnlyArg(const MoveOnlyArg&) = delete;
  MoveOnlyArg(MoveOnlyArg&&) = default;
};

struct TakesMoveOnly {
  int observed;
  explicit TakesMoveOnly(MoveOnlyArg a) : observed(a.v) {}
};

class MemoryTest : public ::testing::Test {
 protected:
  void SetUp() override { Counted::liveCount = 0; }
  void TearDown() override { ASSERT_EQ(Counted::liveCount, 0); }
};

}  // namespace

// ---- makeUniqueNoThrow<T>() -- single object ---------------------------------

TEST_F(MemoryTest, SingleObject_DefaultConstructs) {
  auto p = makeUniqueNoThrow<Counted>();
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->value, 0);
  EXPECT_EQ(Counted::liveCount, 1);
}

TEST_F(MemoryTest, SingleObject_ForwardsSingleArg) {
  auto p = makeUniqueNoThrow<Counted>(42);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->value, 42);
}

TEST_F(MemoryTest, SingleObject_ForwardsMultipleArgs) {
  auto p = makeUniqueNoThrow<Counted>(3, 4);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->value, 7);
}

TEST_F(MemoryTest, SingleObject_ReturnsUniquePtr) {
  auto p = makeUniqueNoThrow<Counted>(1);
  static_assert(std::is_same_v<decltype(p), std::unique_ptr<Counted>>);
}

TEST_F(MemoryTest, SingleObject_DestructorRunsOnScopeExit) {
  {
    auto p = makeUniqueNoThrow<Counted>();
    EXPECT_EQ(Counted::liveCount, 1);
  }
  EXPECT_EQ(Counted::liveCount, 0);
}

TEST_F(MemoryTest, SingleObject_ForwardsRvalueArg) {
  MoveOnlyArg arg{99};
  auto p = makeUniqueNoThrow<TakesMoveOnly>(std::move(arg));
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->observed, 99);
}

TEST_F(MemoryTest, SingleObject_MoveTransfersOwnership) {
  auto p1 = makeUniqueNoThrow<Counted>(7);
  EXPECT_EQ(Counted::liveCount, 1);
  auto p2 = std::move(p1);
  EXPECT_EQ(Counted::liveCount, 1);
  EXPECT_EQ(p1, nullptr);
  ASSERT_NE(p2, nullptr);
  EXPECT_EQ(p2->value, 7);
}

// ---- makeUniqueNoThrow<T[]>(n) -- array form ---------------------------------

TEST_F(MemoryTest, Array_ReturnsUniquePtrArray) {
  auto buf = makeUniqueNoThrow<int[]>(8);
  static_assert(std::is_same_v<decltype(buf), std::unique_ptr<int[]>>);
  ASSERT_NE(buf, nullptr);
}

TEST_F(MemoryTest, Array_ValueInitializesElements) {
  // The `Elem[count]()` syntax zero-initializes trivial types.
  auto buf = makeUniqueNoThrow<int[]>(16);
  ASSERT_NE(buf, nullptr);
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(buf[i], 0) << "index " << i;
  }
}

TEST_F(MemoryTest, Array_DefaultConstructsClassElements) {
  auto buf = makeUniqueNoThrow<Counted[]>(5);
  ASSERT_NE(buf, nullptr);
  EXPECT_EQ(Counted::liveCount, 5);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(buf[i].value, 0);
  }
}

TEST_F(MemoryTest, Array_DestructorsRunOnScopeExit) {
  {
    auto buf = makeUniqueNoThrow<Counted[]>(10);
    ASSERT_NE(buf, nullptr);
    EXPECT_EQ(Counted::liveCount, 10);
  }
  EXPECT_EQ(Counted::liveCount, 0);
}

TEST_F(MemoryTest, Array_WriteableThroughSubscript) {
  auto buf = makeUniqueNoThrow<uint8_t[]>(4);
  ASSERT_NE(buf, nullptr);
  buf[0] = 0xDE;
  buf[3] = 0xAD;
  EXPECT_EQ(buf[0], 0xDE);
  EXPECT_EQ(buf[3], 0xAD);
}

TEST_F(MemoryTest, Array_ZeroSizedAllocationIsValid) {
  // new T[0] is well-defined and returns a non-null pointer; the resulting
  // unique_ptr must still safely destruct.
  auto buf = makeUniqueNoThrow<int[]>(0);
  ASSERT_NE(buf, nullptr);
}

// ---- ScopedCleanup -----------------------------------------------------------

TEST(ScopedCleanupTest, FiresOnScopeExit) {
  int fired = 0;
  {
    ScopedCleanup cleanup{[&] { ++fired; }};
    EXPECT_EQ(fired, 0);
  }
  EXPECT_EQ(fired, 1);
}

TEST(ScopedCleanupTest, FiresExactlyOnce) {
  int fired = 0;
  {
    ScopedCleanup cleanup{[&] { ++fired; }};
  }
  EXPECT_EQ(fired, 1);
}

TEST(ScopedCleanupTest, MultipleCleanupsFireInReverseOrder) {
  // C++ destroys local objects in reverse construction order.
  int sequence[3] = {0, 0, 0};
  int next = 0;
  {
    ScopedCleanup a{[&] { sequence[next++] = 1; }};
    ScopedCleanup b{[&] { sequence[next++] = 2; }};
    ScopedCleanup c{[&] { sequence[next++] = 3; }};
  }
  EXPECT_EQ(sequence[0], 3);
  EXPECT_EQ(sequence[1], 2);
  EXPECT_EQ(sequence[2], 1);
}

TEST(ScopedCleanupTest, CapturesByReferenceForObservableEffect) {
  int counter = 0;
  {
    ScopedCleanup cleanup{[&] { counter = 42; }};
  }
  EXPECT_EQ(counter, 42);
}

namespace {

// Compile-time guarantees: ScopedCleanup is RAII-only -- cannot be copied
// or moved out of its scope. If someone weakens these, the static_asserts
// below fail at build time.
using CleanupT = ScopedCleanup<decltype([] { return 0; })>;
static_assert(!std::is_copy_constructible_v<CleanupT>);
static_assert(!std::is_copy_assignable_v<CleanupT>);
static_assert(!std::is_move_constructible_v<CleanupT>);
static_assert(!std::is_move_assignable_v<CleanupT>);

}  // namespace
