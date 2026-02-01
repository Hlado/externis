#pragma once

#include <memory>

namespace insight {

namespace internal {

class BackendProfilerImpl;

} //namespace internal

struct Options;
class Trace;

class BackendProfiler {
public:
  explicit BackendProfiler(const Options &options, std::shared_ptr<Trace> trace);
  BackendProfiler(BackendProfiler &&);
  BackendProfiler &operator=(BackendProfiler &&);
  ~BackendProfiler();

private:
  std::unique_ptr<internal::BackendProfilerImpl> mImpl;
};

} //namespace insight