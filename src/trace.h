#pragma once

#include "utils.h"

#include <cassert>
#include <deque>
#include <memory>
#include <stack>
#include <string>
#include <unordered_map>

namespace insight {

struct Event {
  std::string name;
  std::chrono::nanoseconds duration;
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

  Trace(Trace &&) = default;
  Trace &operator=(Trace &&) = default;

  const std::string &name() const
  {
    return current().name;
  }

  TimePoint timestamp() const
  {
    return current().timestamp;
  }

  std::chrono::nanoseconds duration() const
  {
    return current().duration;
  }

  // Zero-based, zero after creation
  std::size_t depth() const
  {
    assert(!frontier.empty());
    return frontier.size() - 1;
  }

  void push(std::string name)
  {
    auto &levels = current().levels;
    levels.push_back(Level{std::move(name)});
    frontier.push(&levels.back());
  }

  // Discards the current level along with all its sub-levels and events.
  // No duration from this level will be added to the parent level.
  void drop()
  {
    assert_not_rooted();

    frontier.pop();
  }

  void pop()
  {
    assert_not_rooted();

    auto duration = current().duration;
    frontier.pop();
    current().duration += duration;
  }

  void add(Event event)
  {
    auto &level = current();

    auto duration = event.duration;
    // order is important for exception safety
    level.events.push_back(std::move(event));
    level.duration += duration;
  }

  // Moves all events and levels from another Trace into this level.
  // The other Trace must be at zero depth.
  void merge(Trace &&other)
  {
    if (this == &other) {
      throw Error("can't merge trace with itself");
    }

    auto &level = current();
    level.events.insert(level.events.end(), std::make_move_iterator(other.root->events.begin()),
                        std::make_move_iterator(other.root->events.end()));
    level.levels.insert(level.levels.end(), std::make_move_iterator(other.root->levels.begin()),
                        std::make_move_iterator(other.root->levels.end()));
  }

  // Pops levels until depth is as desired. Does nothing if current depth is already equal or less than desired
  void collapse(std::size_t desiredDepth = 0)
  {
    while (depth() > desiredDepth) {
      pop();
    }
  }

  std::unordered_map<std::string, std::chrono::nanoseconds> flatten() const
  {
    std::unordered_map<std::string, std::chrono::nanoseconds> result;
    auto prefix = std::string{};
    prefix.reserve(2048);

    flatten(result, current(), prefix, 0);

    return result;
  }

private:
  struct Level {
    std::string name;
    TimePoint timestamp{Clock::now()};
    std::chrono::nanoseconds duration{};
    std::deque<Level> levels;
    std::deque<Event> events;
  };

  std::unique_ptr<Level> root{std::make_unique<Level>()};
  // Default allocators always equal, so it's safe to assume that pointers stay the same on move
  std::stack<Level *> frontier;

  bool rooted() const
  {
    return depth() == 0;
  }

  Level &current()
  {
    return const_cast<Level &>(static_cast<const Trace &>(*this).current());
  }

  const Level &current() const
  {
    assert(!frontier.empty());
    return *frontier.top();
  }

  void flatten(std::unordered_map<std::string, std::chrono::nanoseconds> &out,
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
      auto [it, inserted] =
        out.insert(std::make_pair(prefix + normalizeName(event.name), std::chrono::nanoseconds{}));
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
inline void collapse(Trace &trace, std::size_t desiredDepth)
{
  using namespace std::chrono;

  assert(((void)"desired depth must not be greater than trace depth", desiredDepth <= trace.depth()));

  auto now = Clock::now();

  while (trace.depth() >= desiredDepth) {
    auto total = measure(trace.timestamp(), now);
    auto uncategorized = std::max(nanoseconds{}, nanoseconds{total - trace.duration()});

    if (duration_cast<microseconds>(uncategorized) > microseconds{}) {
      trace.add(Event{"Uncategorized", uncategorized});
    }

    if (trace.depth() == desiredDepth) {
      break;
    }

    trace.pop();
  };
}

} // namespace insight
