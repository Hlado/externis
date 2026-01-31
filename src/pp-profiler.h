#pragma once

#include <memory>

namespace insight {

namespace internal {

class PpProfilerImpl;

} //namespace internal

struct Options;
struct Stage;

class PpProfiler {
public:
  explicit PpProfiler(const Options &options);
  PpProfiler(PpProfiler &&);
  PpProfiler &operator=(PpProfiler &&);
  ~PpProfiler();

  void dump(Stage &sink) const;

private:
  std::unique_ptr<internal::PpProfilerImpl> mImpl;
};

} //namespace insight