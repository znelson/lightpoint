// Host-test implementations for the real lib/hal/HalPlatform.h. Only methods
// actually referenced by test-linked code get defined here -- everything else
// stays declared-but-undefined. A new test path that exercises another
// HalPlatform method will fail to link with an undefined-symbol error, which
// is the signal to add another stub line below.

#include <HalPlatform.h>

HalPlatform halPlatform;

uint32_t HalPlatform::freeHeap() const { return 256u * 1024u; }
