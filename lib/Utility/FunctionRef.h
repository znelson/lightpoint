#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

// Non-owning, type-erased reference to a callable.
//
// Holds a pointer to the callable plus a function-pointer trampoline; two
// pointers total, zero heap allocation. The caller MUST keep the underlying
// callable alive for the lifetime of the FunctionRef.
//
// Use this to give a non-template receiver a generic callable parameter
// without paying the template-instantiation-per-call-site cost of
// `template<typename Fn> void receiver(Fn&&)`, or the heap-allocation +
// per-signature binary bloat of `std::function`.
//
// Example:
//   void forEachItem(FunctionRef<void(const Item&)> visit) {
//     for (const Item& it : items) visit(it);
//   }
//   forEachItem([&](const Item& it) { total += it.size; });
//
// Null state: default-constructed or nullptr-constructed FunctionRefs are
// empty; `if (cb) cb(...)` is the safe-call idiom. Calling operator() on
// an empty FunctionRef is undefined behavior.

template <typename Signature>
class FunctionRef;

template <typename R, typename... Args>
class FunctionRef<R(Args...)> {
 public:
  FunctionRef() = default;
  FunctionRef(std::nullptr_t) noexcept {}

  // Implicit converting constructor: callers pass lambdas / function objects
  // directly, matching std::function's ergonomics. noExplicitConstructor is
  // suppressed file-wide in platformio.ini (covers both this and the
  // std::nullptr_t empty-state constructor above).
  template <typename Fn>
    requires(!std::same_as<std::remove_cvref_t<Fn>, FunctionRef> && std::is_invocable_r_v<R, Fn&, Args...>)
  FunctionRef(Fn&& fn) noexcept
      : _obj(reinterpret_cast<intptr_t>(&fn)), _call(&trampoline<std::remove_reference_t<Fn>>) {}

  R operator()(Args... args) const { return _call(_obj, std::forward<Args>(args)...); }

  explicit operator bool() const noexcept { return _call != nullptr; }
  bool operator==(std::nullptr_t) const noexcept { return _call == nullptr; }

 private:
  template <typename Fn>
  static R trampoline(intptr_t obj, Args... args) {
    return (*reinterpret_cast<Fn*>(obj))(std::forward<Args>(args)...);
  }

  intptr_t _obj = 0;
  R (*_call)(intptr_t, Args...) = nullptr;
};
