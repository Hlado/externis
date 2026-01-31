#pragma once

#include "utils.h"

#include <iosfwd>
#include <stack>
#include <string>
#include <unordered_map>

namespace insight {

struct Stage
{
  using Records = std::unordered_map<std::string, std::chrono::nanoseconds>;

  TimePoint start;
  std::string name;
  Records records;

  //TODO: misplacement
  static void collapse(std::stack<Stage> &stages, std::size_t desiredDepth);

  void consume(const Stage &other);
  std::chrono::nanoseconds duration() const;

  //TODO: misplacement
  void dump(std::ostream &stream) const;
};

} //namespace insight