file(GLOB SB_TEST_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/support/*.cpp)

add_executable(storage_bridge_test
  ${SB_TEST_SOURCES}
  ${CMAKE_SOURCE_DIR}/src/web/bootstrap.cpp
  ${SB_CONTROLLER_SOURCES})

target_include_directories(storage_bridge_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(storage_bridge_test PRIVATE
  storage_bridge_core
  GTest::gtest
  GTest::gmock)

target_compile_options(storage_bridge_test PRIVATE
  -Wall -Wextra -Wno-unused-parameter)

target_compile_definitions(storage_bridge_test PRIVATE
  SB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

set_target_properties(storage_bridge_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/test)

gtest_discover_tests(storage_bridge_test
  TEST_FILTER "Unit*"
  TEST_LIST SB_UNIT_TESTS
  PROPERTIES LABELS "unit"
  DISCOVERY_TIMEOUT 30)

gtest_discover_tests(storage_bridge_test
  TEST_FILTER "Integration*"
  TEST_LIST SB_INTEGRATION_TESTS
  PROPERTIES LABELS "integration"
  DISCOVERY_TIMEOUT 30)