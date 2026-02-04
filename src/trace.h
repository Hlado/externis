#pragma once

#include "utils.h"

#include <cassert>
#include <list>
#include <memory>
#include <stack>
#include <string>
#include <unordered_map>

namespace insight {

struct Event {
  std::string name;
  NanosecondsFp duration;
};

// The Trace class consists of 'levels' and 'events'.
// Events are added to the current level, and new levels can be pushed.
// Once a level is popped, it becomes immutable.
// Most query methods report information about the current level.
//
// Each level records its creation timestamp.
// For the implicit root level, this timestamp is the object construction time.
//
// The duration of a level is computed as the cumulative duration of its events
// plus the durations of all sub-levels.
// - event durations are added as events are inserted.
// - sub-level durations are added when the sub-level is popped.
class Trace {
public:
  Trace()
  {
    frontier.push(root.get());
  }

  Trace(Trace &&other) = delete;
  Trace &operator=(Trace &&) = delete;

  const std::string &name() const noexcept
  {
    return current().name;
  }

  const TimePoint &timestamp() const noexcept
  {
    return current().timestamp;
  }

  const NanosecondsFp &duration() const noexcept
  {
    return current().duration;
  }

  // Zero-based, zero after creation
  std::size_t depth() const noexcept
  {
    assert(!frontier.empty());
    return frontier.size() - 1;
  }

  void add(Event event)
  {
    auto &level = current();

    auto duration = event.duration;
    // order is important for exception safety
    level.events.push_back(std::move(event));
    level.duration += duration;
  }

  void push(std::string name)
  {
    auto &levels = current().levels;
    levels.push_back(Level{std::move(name)});
    frontier.push(&levels.back());
  }

  // Noexcept if not rooted
  void pop()
  {
    assert_not_rooted();

    auto duration = current().duration;
    frontier.pop(); // assumed to be noexcept
    current().duration += duration;
  }

  // Pops levels until depth is as desired. Does nothing if current depth is already equal or less than desired
  void collapse(std::size_t desiredDepth = 0) noexcept
  {
    while (depth() > desiredDepth) {
      pop();
    }
  }

  // Discards the current level along with all its sub-levels and events.
  // No duration from this level will be added to the parent level.
  // Noexcept if not rooted
  void drop()
  {
    assert_not_rooted();

    frontier.pop();
  }

  // Dropss levels until depth is as desired. Does nothing if current depth is already equal or less than desired
  void discard(std::size_t desiredDepth = 0) noexcept
  {
    while (depth() > desiredDepth) {
      drop();
    }
  }

  // Collapse other trace and steal it's levels and events to current level
  void consume(Trace &&other) noexcept
  {
    other.collapse();

    auto &level = current();
    level.events.splice(level.events.end(), other.current().events);
    level.levels.splice(level.levels.end(), other.current().levels);
    level.duration += other.duration();
    other.current().duration = NanosecondsFp{};
  }

  std::unordered_map<std::string, NanosecondsFp> flatten() const
  {
    std::unordered_map<std::string, NanosecondsFp> result;
    auto prefix = std::string{};
    prefix.reserve(2048);

    flatten(result, current(), prefix, 0);

    return result;
  }

private:
  struct Level {
    std::string name;
    TimePoint timestamp{Clock::now()};
    NanosecondsFp duration{};
    std::list<Level> levels;
    std::list<Event> events;
  };

  std::unique_ptr<Level> root{std::make_unique<Level>()};
  // List elements and iterators invalidatet only on erasing, so we can hold pointer safely
  std::stack<Level *> frontier;

  bool rooted() const noexcept
  {
    return depth() == 0;
  }

  Level &current() noexcept
  {
    return const_cast<Level &>(static_cast<const Trace &>(*this).current());
  }

  const Level &current() const noexcept
  {
    assert(!frontier.empty());
    return *frontier.top();
  }

  void flatten(std::unordered_map<std::string, NanosecondsFp> &out,
               const Level &level,
               std::string &prefix,
               std::size_t depth) const
  {
    if (depth >= 1000) {
      throw Error("recursion depth exceeded limit");
    }

    std::size_t prefixLen{prefix.size()};
    if (&level != &current()) {
      prefix += level.name + ";";
    }

    for (auto &&event : level.events) {
      auto [it, inserted] = out.insert(std::make_pair(prefix + normalizeName(event.name), NanosecondsFp{}));
      it->second += event.duration;
    }

    for (auto &&level : level.levels) {
      flatten(out, level, prefix, depth + 1);
    }

    prefix.resize(prefixLen);
  }

  void assert_rooted() const
  {
    if (!rooted()) {
      throw Error("depth is not zero");
    }
  }

  void assert_not_rooted() const
  {
    if (rooted()) {
      throw Error("depth is zero");
    }
  }
};

// Temporary helper
inline void collapse(Trace &trace, std::size_t desiredDepth = 0)
{
  using namespace std::chrono;

  assert(((void)"desired depth must not be greater than trace depth", desiredDepth <= trace.depth()));

  auto now = Clock::now();

  while (trace.depth() >= desiredDepth) {
    auto total = measure(trace.timestamp(), now);
    auto uncategorized = std::max(NanosecondsFp{}, NanosecondsFp{total - trace.duration()});

    if (MicrosecondsFp{uncategorized} > MicrosecondsFp{0.5}) {
      trace.add(Event{"Uncategorized", uncategorized});
    }

    if (trace.depth() == desiredDepth) {
      break;
    }

    trace.pop();
  };
}

} // namespace insight
