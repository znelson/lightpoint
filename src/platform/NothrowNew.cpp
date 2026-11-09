// Replacement nothrow allocation functions.
//
// libstdc++ implements operator new(size_t, nothrow_t) as a try/catch around
// the throwing operator new. With CONFIG_COMPILER_CXX_EXCEPTIONS=n, ESP-IDF
// wraps __cxa_allocate_exception/__cxa_throw with abort() stubs
// (components/cxx/cxx_exception_stubs.cpp), so the throwing path aborts before
// the catch can return nullptr: new (std::nothrow) aborts on OOM exactly like
// bare new does.
//
// These definitions replace the libstdc++ ones program-wide (they are
// standard-replaceable functions) and go straight to malloc, so
// new (std::nothrow), makeUniqueNoThrow and makeSharedNoThrow really do return
// nullptr on allocation failure. libstdc++'s operator delete calls free(), so
// the matching deallocation is unchanged.
//
// Not covered: the over-aligned (std::align_val_t) nothrow forms, which the
// firmware does not use.

#include <cstddef>
#include <cstdlib>
#include <new>

void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return malloc(size ? size : 1); }

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return malloc(size ? size : 1); }
