#include "stage.h"

#include <cassert>
#include <numeric>

using namespace std::chrono;

namespace insight {

void Stage::collapse(std::stack<Stage> &stages, std::size_t desiredDepth)
{
  assert(((void)"desired depth must be positive", desiredDepth > 0));
  assert(((void)"desired depth must not be greater than stack size", desiredDepth <= stages.size()));

  auto now = Clock::now();

  while(stages.size() > desiredDepth)
  {
    auto stage = std::move(stages.top());
    stages.pop();

    auto total = measure(stage.start, now);
    stage.records["Uncategorized"] = std::max(nanoseconds::zero(), nanoseconds{total - stage.duration()});

    stages.top().consume(stage);
  };
}

void Stage::consume(const Stage &other)
{
  if(this == &other) {
    for(auto &[n, d] : records) {
      d *= 2;
    }
    return;
  }

  for(auto &&[n, d] : other.records) {
    std::string fullName{other.name + ";" + n};
    auto it = records.find(name);
    if(it == records.end()) {
      auto [newIt, inserted] = records.emplace(std::move(fullName), nanoseconds::zero());
      it = newIt;
    }
    it->second += d;
  }
}

nanoseconds Stage::duration() const
{
  using Pair = Records::value_type;
  return std::accumulate(records.cbegin(), records.cend(), nanoseconds::zero(), [](nanoseconds acc, const Pair &r) { return acc + r.second; });
}

void Stage::dump(std::ostream &stream) const
{
  for(auto &&[n, d] : records) {
    stream << name << ";" << n << " " << duration_cast<microseconds>(d).count() << "\n";
  }
  stream.flush();
}

} //namespace insight