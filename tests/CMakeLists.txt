find_package(GTest REQUIRED)
include(GoogleTest)

# Unit tests: no network, no database.
add_executable(sema_unit_tests
  test_state_vector.cpp
  test_geo.cpp
  test_military_filter.cpp
  test_opensky_client.cpp
  test_pipeline.cpp
)
target_link_libraries(sema_unit_tests PRIVATE sema_core GTest::gtest_main)
target_compile_definitions(sema_unit_tests PRIVATE
  SEMA_DATA_DIR="${PROJECT_SOURCE_DIR}/data"
  SEMA_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
gtest_discover_tests(sema_unit_tests PROPERTIES LABELS unit)

# Integration tests: need SEMA_TEST_DATABASE_URL pointing at a PostGIS database.
add_executable(sema_integration_tests test_pg_store.cpp)
target_link_libraries(sema_integration_tests PRIVATE sema_core GTest::gtest_main)
gtest_discover_tests(sema_integration_tests PROPERTIES LABELS integration)
