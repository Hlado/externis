#include "gcc-utils.h"

#include <langhooks.h>

#include <cassert>
#include <unordered_map>

namespace insight {

std::string getFunctionId(tree decl, Verbosity verbosity)
{
  static const auto mapping = std::unordered_map<Verbosity, int> {
    {Verbosity::Minimal, 0},
    {Verbosity::Default, 1},
    {Verbosity::Detailed, 2}
  };

  assert(decl != nullptr);
  assert(TREE_CODE(decl) == FUNCTION_DECL);
  
  return lang_hooks.decl_printable_name(decl, mappedValue(mapping, verbosity));
}

} //namespace insight