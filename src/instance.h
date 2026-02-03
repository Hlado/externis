#pragma once

#include <memory>

namespace insight {

namespace internal {

class InstanceImpl;

} // namespace internal

struct Options;

class Instance {
public:
  explicit Instance(const Options &options);
  Instance(Instance &&) = delete;
  Instance &operator=(Instance &&) = delete;
  ~Instance();

private:
  std::unique_ptr<internal::InstanceImpl> mImpl;
};

} // namespace insight
