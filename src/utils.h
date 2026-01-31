#pragma once

#include "insight.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <sstream>

namespace insight {

// We use a chrono-provided clock for initial simplicity (and can introduce a
// custom clock later if needed). For now, we favor accuracy over monotonicity:
// non-monotonic behavior is unlikely to cause critical issues in practice for
// our purposes. But this may change in the future.
using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

class Error : public std::runtime_error {
public:
  template <typename... T>
  explicit Error(T &&... args)
    : std::runtime_error(static_cast<const std::stringstream &>((std::stringstream{} << ... << std::forward<T>(args))).str())
  {

  }
};

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

template <void(*HandlerV)(void*, void*)>
void handleCallback(void *gccData, void *userData) noexcept {
  try {
    HandlerV(gccData, userData);
  } catch(const std::exception &e) {
    logError(e.what());
  } catch(...) {
    logError("unknown error");
  }
}

inline std::chrono::nanoseconds measure(TimePoint then, TimePoint now)
{
  using namespace std::chrono;

  //Clock isn't guaranteed to be steady
  return std::max(nanoseconds::zero(), nanoseconds{now - then});
}

} // namespace insight
