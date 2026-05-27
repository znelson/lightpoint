#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "FunctionRef.h"

namespace {

int doubleIt(int x) { return x * 2; }
void voidSink(int& dst, int v) { dst = v; }

struct Functor {
  int factor;
  int operator()(int x) const { return x * factor; }
};

struct MutatingFunctor {
  int calls = 0;
  int operator()() {
    ++calls;
    return calls;
  }
};

}  // namespace

// ---- size / shape ------------------------------------------------------------

TEST(FunctionRefSize, IsTwoPointers) {
  // The whole point: no heap, no SBO -- just object pointer + trampoline.
  EXPECT_EQ(sizeof(FunctionRef<void()>), 2 * sizeof(void*));
  EXPECT_EQ(sizeof(FunctionRef<int(int, int)>), 2 * sizeof(void*));
}

// ---- capture-less lambdas ----------------------------------------------------

TEST(FunctionRefLambda, CaptureLessLambdaReturnsValue) {
  auto plusOne = [](int x) { return x + 1; };
  FunctionRef<int(int)> ref = plusOne;
  EXPECT_EQ(ref(41), 42);
}

TEST(FunctionRefLambda, MultipleInvocationsConsistent) {
  auto square = [](int x) { return x * x; };
  FunctionRef<int(int)> ref = square;
  EXPECT_EQ(ref(2), 4);
  EXPECT_EQ(ref(5), 25);
  EXPECT_EQ(ref(7), 49);
}

// ---- capturing lambdas -------------------------------------------------------

TEST(FunctionRefLambda, CapturingLambdaSeesEnclosingState) {
  int multiplier = 3;
  auto times = [&](int x) { return x * multiplier; };
  FunctionRef<int(int)> ref = times;
  EXPECT_EQ(ref(4), 12);
  multiplier = 10;
  EXPECT_EQ(ref(4), 40);
}

TEST(FunctionRefLambda, CapturingLambdaCanWriteThroughReference) {
  int sink = 0;
  auto setter = [&](int v) { sink = v; };
  FunctionRef<void(int)> ref = setter;
  ref(99);
  EXPECT_EQ(sink, 99);
  ref(-1);
  EXPECT_EQ(sink, -1);
}

// ---- const callables ---------------------------------------------------------

TEST(FunctionRefConst, BindsToConstLambda) {
  const auto identity = [](int x) { return x; };
  FunctionRef<int(int)> ref = identity;
  EXPECT_EQ(ref(123), 123);
}

TEST(FunctionRefConst, BindsToConstReference) {
  auto lambda = [](int x) { return x + 5; };
  const auto& cref = lambda;
  FunctionRef<int(int)> ref = cref;
  EXPECT_EQ(ref(10), 15);
}

// ---- function pointers -------------------------------------------------------

TEST(FunctionRefFnPtr, BindsToFreeFunction) {
  FunctionRef<int(int)> ref = doubleIt;
  EXPECT_EQ(ref(21), 42);
}

TEST(FunctionRefFnPtr, BindsToFunctionPointerVariable) {
  int (*fp)(int) = &doubleIt;
  FunctionRef<int(int)> ref = fp;
  EXPECT_EQ(ref(3), 6);
}

// ---- functor objects ---------------------------------------------------------

TEST(FunctionRefFunctor, BindsToConstFunctor) {
  Functor f{5};
  FunctionRef<int(int)> ref = f;
  EXPECT_EQ(ref(4), 20);
}

TEST(FunctionRefFunctor, MutableFunctorStateAdvances) {
  MutatingFunctor f;
  FunctionRef<int()> ref = f;
  EXPECT_EQ(ref(), 1);
  EXPECT_EQ(ref(), 2);
  EXPECT_EQ(ref(), 3);
  EXPECT_EQ(f.calls, 3);
}

// ---- argument forwarding -----------------------------------------------------

TEST(FunctionRefArgs, ForwardsMultipleArgs) {
  auto add = [](int a, int b, int c) { return a + b + c; };
  FunctionRef<int(int, int, int)> ref = add;
  EXPECT_EQ(ref(1, 2, 3), 6);
}

TEST(FunctionRefArgs, ForwardsReferenceArgs) {
  int dst = 0;
  FunctionRef<void(int&, int)> ref = voidSink;
  ref(dst, 77);
  EXPECT_EQ(dst, 77);
}

TEST(FunctionRefArgs, ForwardsLargeValueArg) {
  // std::string copy through the parameter -- exercises forwarding for
  // a non-trivially-copyable type.
  auto sizeOf = [](std::string s) { return s.size(); };
  FunctionRef<size_t(std::string)> ref = sizeOf;
  EXPECT_EQ(ref(std::string("hello")), 5u);
}

// ---- void return -------------------------------------------------------------

TEST(FunctionRefVoid, VoidReturningLambda) {
  int counter = 0;
  auto inc = [&] { ++counter; };
  FunctionRef<void()> ref = inc;
  ref();
  ref();
  ref();
  EXPECT_EQ(counter, 3);
}

// ---- usage pattern: forEach receiver -----------------------------------------

namespace {

// A non-template receiver that accepts any compatible callable. This is the
// motivating use case: forEach stays in a .cpp file (no per-call-site
// instantiation), and only a small trampoline stub is generated per caller.
void visitInts(const std::vector<int>& xs, FunctionRef<void(int)> visit) {
  for (int x : xs) visit(x);
}

int sumInts(const std::vector<int>& xs, FunctionRef<int(int, int)> combine) {
  int acc = 0;
  for (int x : xs) acc = combine(acc, x);
  return acc;
}

}  // namespace

TEST(FunctionRefForEach, VisitsEachElement) {
  std::vector<int> xs{1, 2, 3, 4, 5};
  std::vector<int> seen;
  visitInts(xs, [&](int x) { seen.push_back(x); });
  EXPECT_EQ(seen, xs);
}

TEST(FunctionRefForEach, ReducesWithCallable) {
  std::vector<int> xs{1, 2, 3, 4, 5};
  int total = sumInts(xs, [](int a, int b) { return a + b; });
  EXPECT_EQ(total, 15);
}

TEST(FunctionRefForEach, AcceptsFunctionPointer) {
  std::vector<int> xs{10, 20, 30};
  int total = sumInts(xs, +[](int a, int b) { return a + b; });
  EXPECT_EQ(total, 60);
}

// ---- copy semantics ----------------------------------------------------------

TEST(FunctionRefCopy, IsTriviallyCopyable) {
  // FunctionRef is a value type with two trivial members; it should be
  // freely copyable and behave like a pair of pointers.
  static_assert(std::is_trivially_copyable_v<FunctionRef<void()>>);
  static_assert(std::is_trivially_destructible_v<FunctionRef<void()>>);
}

TEST(FunctionRefCopy, CopyRefersToSameUnderlyingCallable) {
  int counter = 0;
  auto bump = [&] { ++counter; };
  FunctionRef<void()> a = bump;
  FunctionRef<void()> b = a;
  a();
  b();
  EXPECT_EQ(counter, 2);
}

// ---- SFINAE: self-binding -------------------------------------------------

namespace {

template <typename T, typename Fn, typename = void>
struct IsConstructibleFromCallable : std::false_type {};

template <typename T, typename Fn>
struct IsConstructibleFromCallable<T, Fn, std::void_t<decltype(T(std::declval<Fn>()))>> : std::true_type {};

// Plain pointer to int is not invocable -- SFINAE should reject it.
static_assert(!IsConstructibleFromCallable<FunctionRef<int(int)>, int*>::value);

// A lambda with the wrong arity should be rejected.
static_assert(!IsConstructibleFromCallable<FunctionRef<int(int)>, decltype([] { return 0; })>::value);

// A lambda with the right shape should be accepted.
static_assert(IsConstructibleFromCallable<FunctionRef<int(int)>, decltype([](int x) { return x; })>::value);

}  // namespace
