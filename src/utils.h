#pragma once

#include "insight.h"

#include <chrono>
#include <iostream>

namespace insight {

// We use a chrono-provided clock for initial simplicity (and can introduce a
// custom clock later if needed). For now, we favor accuracy over monotonicity:
// non-monotonic behavior is unlikely to cause critical issues in practice for
// our purposes. But this may change in the future.
using Clock = std::chrono::high_resolution_clock;

template <typename... T>
void logInfo(T... args) {
  ((std::cout << PLUGIN_NAME << ": ") << ... << args) << "\n";
}

template <typename... T>
void logWarn(T... args) {
  ((std::cerr << PLUGIN_NAME << ": [WARN] ") << ... << args) << "\n";
}

template <typename... T>
void logError(T... args) {
  ((std::cerr << PLUGIN_NAME << ": [ERROR] ") << ... << args) << "\n";
}

} // namespace insight
