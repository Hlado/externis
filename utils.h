#pragma once

#include <chrono>
#include <iostream>

namespace insight {

// We use a chrono-provided clock for initial simplicity (and can introduce a
// custom clock later if needed). For now, we favor accuracy over monotonicity:
// non-monotonic behavior is unlikely to cause critical issues in practice for
// our purposes. But this may change in the future.
using Clock = std::chrono::high_resolution_clock;

inline auto &logStream = std::cout;

template <typename... T>
void log(T... args) {
  (logStream << ... << args) << std::endl;
}

} // namespace insight