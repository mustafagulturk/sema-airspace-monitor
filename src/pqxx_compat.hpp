// Include libpqxx through this header.
//
// libpqxx switches its exception constructors to std::source_location when the
// *including* translation unit has C++20 source_location. Distribution builds of
// libpqxx 7.8 (e.g. Ubuntu 24.04's libpqxx-dev) are compiled as C++17, so those
// constructors do not exist in the shared library and C++20 code fails to link.
// Hide source_location from libpqxx in that case. Override with
// -DSEMA_PQXX_SOURCE_LOCATION=0/1 if the heuristic is wrong for your build.
#pragma once

#include <version>

#include <pqxx/version>

#ifndef SEMA_PQXX_SOURCE_LOCATION
#  if PQXX_VERSION_MAJOR == 7 && PQXX_VERSION_MINOR < 9
#    define SEMA_PQXX_SOURCE_LOCATION 0
#  else
#    define SEMA_PQXX_SOURCE_LOCATION 1
#  endif
#endif

#if !SEMA_PQXX_SOURCE_LOCATION
#  undef __cpp_lib_source_location
#endif

#include <pqxx/pqxx>
