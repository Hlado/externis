#pragma once

#include <memory>

namespace insight {

namespace internal {

class ParserProfilerImpl;

} // namespace internal

struct Options;
class Trace;

class ParserProfiler {
public:
  explicit ParserProfiler(const Options &options, std::shared_ptr<Trace> trace);
  ParserProfiler(ParserProfiler &&) = delete;
  ParserProfiler &operator=(ParserProfiler &&) = delete;
  ~ParserProfiler();

private:
  std::unique_ptr<internal::ParserProfilerImpl> mImpl;
};

} // namespace insight
