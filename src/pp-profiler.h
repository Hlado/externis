#pragma once

#include <functional>
#include <memory>

namespace insight {

namespace internal {

class PpProfilerImpl;

} // namespace internal

struct Options;
class Trace;

class PpProfiler {
public:
  PpProfiler(const Options &options, std::shared_ptr<Trace> trace, std::function<void()> finishHandler = {});
  PpProfiler(PpProfiler &&) = delete;
  PpProfiler &operator=(PpProfiler &&) = delete;
  ~PpProfiler();

private:
  std::unique_ptr<internal::PpProfilerImpl> mImpl;
};

} // namespace insight
