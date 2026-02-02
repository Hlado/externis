#pragma once

// Weird thing, without that gcc 9 goes insane with compilation errors.
// Looks like some system header is missing
#if __GNUC__ == 9
#include <filesystem>
#endif

// gcc headers have to be quarantined due to surprises like #define abort
//
// Avoid including it from headers

// Has to be included first
#include <gcc-plugin.h>

#include <cpplib.h>
#include <plugin-version.h>

#include <system.h>
#include <tree-pass.h>
#include <tree.h>

#undef abort
