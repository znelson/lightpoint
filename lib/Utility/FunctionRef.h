#pragma once

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

template <typename Signature>
class FunctionRef;

template <typename R, typename... Args>
class FunctionRef<R(Args...)> {
 public:
  template <typename Fn,
            typename = std::enable_if_t<
                !std::is_same_v<std::remove_cvref_t<Fn>, FunctionRef> &&
                std::is_invocable_r_v<R, Fn&, Args...>>>
  FunctionRef(Fn&& fn) noexcept
      : _obj(reinterpret_cast<intptr_t>(&fn)),
        _call(&trampoline<std::remove_reference_t<Fn>>) {}

  R operator()(Args... args) const {
    return _call(_obj, std::forward<Args>(args)...);
  }

 private:
  template <typename Fn>
  static R trampoline(intptr_t obj, Args... args) {
    return (*reinterpret_cast<Fn*>(obj))(std::forward<Args>(args)...);
  }

  intptr_t _obj;
  R (*_call)(intptr_t, Args...);
};
