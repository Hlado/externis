#pragma once

#include <memory>

namespace insight {

namespace internal {

class BackendProfilerImpl;

} // namespace internal

struct Options;
class Trace;

class BackendProfiler {
public:
  explicit BackendProfiler(const Options &options, std::shared_ptr<Trace> trace);
  BackendProfiler(BackendProfiler &&) = delete;
  BackendProfiler &operator=(BackendProfiler &&) = delete;
  ~BackendProfiler();

  void handlePassExecution(void *gccData);
  // Rely on plugin finish only may make last pass measurement inaccurate,
  // but it looks like goog trade-off for simplicity
  void handlePluginFinish(void *gccData);

private:
  std::unique_ptr<internal::BackendProfilerImpl> mImpl;
};

} // namespace insight
