// Randomised property tests, run by `turbocryptosat selftest`. See selftest.cpp
// for what is checked and why the benchmark cannot check it.
#pragma once

#include <cstdint>

namespace tcs {

// Returns 0 when every round passed, 1 on the first failure - which prints the
// seed that reproduces it. `rounds <= 0` picks a default.
int runSelfTest(uint64_t seed, int rounds);

}  // namespace tcs
