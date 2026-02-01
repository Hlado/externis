#pragma once

#include <string>

//Always last
#include "gcc-headers.h"

namespace insight {

std::string getFunctionQualifiedId(tree decl)
{
  location_t loc = DECL_SOURCE_LOCATION(decl);
  unsigned int line = 0;
  unsigned int column = 0;
  if (loc != UNKNOWN_LOCATION) {
    line = LOCATION_LINE(loc);
    column = LOCATION_COLUMN(loc);
  }
  return decl ? IDENTIFIER_POINTER(DECL_NAME(decl)) : std::string("<anonymous>:") + std::to_string(line) + ":" + std::to_string(column);
}

} //namespace insight