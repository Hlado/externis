#pragma once

#include "insight.h"

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace insight {

// We use a chrono-provided clock for initial simplicity (and can introduce a
// custom clock later if needed). For now, we favor accuracy over monotonicity:
// non-monotonic behavior is unlikely to cause critical issues in practice for
// our purposes. But this may change in the future.
using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

enum class Verbosity { Minimal, Default, Detailed };

class Error : public std::runtime_error {
public:
  template <typename... T>
  explicit Error(T &&...args)
  : std::runtime_error(
      static_cast<const std::stringstream &>((std::stringstream{} << ... << std::forward<T>(args))).str())
  {
  }
};

template <typename... T>
void logInfo(T... args)
{
  ((std::cout << PLUGIN_NAME << ": ") << ... << args) << "\n";
}

template <typename... T>
void logWarn(T... args)
{
  ((std::cerr << PLUGIN_NAME << ": [WARN] ") << ... << args) << "\n";
}

template <typename... T>
void logError(T... args)
{
  ((std::cerr << PLUGIN_NAME << ": [ERROR] ") << ... << args) << "\n";
}

template <void (*HandlerV)(void *, void *)>
void handleCallback(void *gccData, void *userData) noexcept
{
  try {
    HandlerV(gccData, userData);
  } catch (const std::exception &e) {
    logError(e.what());
  } catch (...) {
    logError("unknown error");
  }
}

inline std::chrono::nanoseconds measure(TimePoint then, TimePoint now)
{
  using namespace std::chrono;

  // Clock isn't guaranteed to be steady
  return std::max(nanoseconds::zero(), nanoseconds{now - then});
}

inline void dump(std::unordered_map<std::string, std::chrono::nanoseconds> &collapsed, std::ostream &stream)
{
  using namespace std::chrono;

  for (auto &&[n, d] : collapsed) {
    stream << n << " " << duration_cast<microseconds>(d).count() << "\n";
  }
  stream.flush();
}

template <typename AssociativeContainerT>
auto mappedValue(const AssociativeContainerT &mapping, const typename AssociativeContainerT::key_type &key)
{
  auto it = mapping.find(key);
  if (it == std::end(mapping)) {
    throw Error("unknown mapping");
  }
  return it->second;
}

// Some name strings may contain semicolons, which are normally used as separators in
// .collapsed output. At the same time, we sometimes want to "squash" multiple trace
// levels into a single name string. To avoid inspection of names at profiling step,
// we use a special character unlikely to appear in normal names as an internal
// separator. The final string processing will be done at trace flattening step.
constexpr auto LSEP = char{0x1F}; // Unit Separator (ASCII control character)
const auto LSEP_STR = std::string{LSEP};

// Squashes multiple levels in one string using special separators
inline std::string squashLevels(std::initializer_list<std::string> names)
{
  if (names.size() == 0) {
    return {};
  }

  auto result = *names.begin();
  for (auto it = std::next(names.begin()); it != names.end(); ++it) {
    result += LSEP + *it;
  }

  return result;
}

// Replaces semicolons with bars '|' (not ideal but fast and fair solution),
// and separator characters with semicolon
inline std::string normalizeName(std::string name)
{
  for (auto &c : name) {
    switch (c) {
    case ';':
      c = '|';
      break;
    case LSEP:
      c = ';';
      break;
    };
  }

  return name;
}

} // namespace insight
