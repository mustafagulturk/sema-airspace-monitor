cmake_minimum_required(VERSION 3.25)
project(sema VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SEMA_BUILD_TESTS "Build tests" ON)
option(SEMA_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
set(SEMA_SANITIZER "" CACHE STRING "Sanitizer to enable: address, thread or empty")

# --- Dependencies (vcpkg manifest or system packages) ----------------------
find_package(CURL REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
find_package(libpqxx CONFIG QUIET)
if(NOT TARGET libpqxx::pqxx)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx)
  add_library(libpqxx::pqxx ALIAS PkgConfig::PQXX)
endif()
find_package(Threads REQUIRED)

# --- Common compile options -------------------------------------------------
add_library(sema_options INTERFACE)
target_compile_options(sema_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(SEMA_WARNINGS_AS_ERRORS)
  target_compile_options(sema_options INTERFACE -Werror)
endif()
if(SEMA_SANITIZER STREQUAL "address")
  target_compile_options(sema_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(sema_options INTERFACE -fsanitize=address,undefined)
elseif(SEMA_SANITIZER STREQUAL "thread")
  target_compile_options(sema_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(sema_options INTERFACE -fsanitize=thread)
elseif(NOT SEMA_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown SEMA_SANITIZER '${SEMA_SANITIZER}'")
endif()

# --- Embed SQL migration into the binary -----------------------------------
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/sql/001_init.sql SEMA_SQL_001_INIT)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS sql/001_init.sql)
configure_file(src/migrations.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/sema/migrations.hpp @ONLY)

# --- Core library ------------------------------------------------------------
add_library(sema_core
  src/state_vector.cpp
  src/geo.cpp
  src/aircraft_registry.cpp
  src/military_filter.cpp
  src/curl_transport.cpp
  src/opensky_client.cpp
  src/pipeline.cpp
  src/pg_store.cpp
  src/clock.cpp
  src/kalman.cpp
  src/quality.cpp
  src/metrics.cpp
  src/tracker.cpp
  src/snapshot_io.cpp
)
target_include_directories(sema_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(sema_core
  PUBLIC nlohmann_json::nlohmann_json Threads::Threads sema_options
  PRIVATE CURL::libcurl libpqxx::pqxx)

add_executable(sema_collector apps/collector.cpp)
target_link_libraries(sema_collector PRIVATE sema_core)

add_executable(sema_replay apps/replay.cpp)
target_link_libraries(sema_replay PRIVATE sema_core)

if(SEMA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
