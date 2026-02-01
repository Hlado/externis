#pragma once

#include <memory>

namespace insight {

namespace internal {

class BackendProfilerImpl;

} //namespace internal

struct Options;
struct Stage;

class BackendProfiler {
public:
  explicit BackendProfiler(const Options &options);
  BackendProfiler(BackendProfiler &&);
  BackendProfiler &operator=(BackendProfiler &&);
  ~BackendProfiler();

  void dump(Stage &sink) const;

private:
  std::unique_ptr<internal::BackendProfilerImpl> mImpl;
};

} //namespace insight