cmake_minimum_required(VERSION 3.16)
project(storage-bridge
  VERSION 0.1.0
  DESCRIPTION "Bridge between WebDAV storage and a remote PostgreSQL catalog"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ----------------------------------------------------------------------------- formatting
# 手动格式化：cmake --build build --target format
find_program(CLANG_FORMAT_EXE NAMES clang-format clang-format-18 clang-format-17)

# ----------------------------------------------------------------------------- dependencies
# 全部依赖由 vcpkg manifest 模式提供（见 vcpkg.json + CMakePresets.json）。
# 手动构建时需指定工具链：-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

find_package(Drogon CONFIG REQUIRED)          # HTTP 框架 + trantor 日志
find_package(CURL CONFIG REQUIRED)            # WebDAV 客户端传输层
find_package(SOCI CONFIG REQUIRED COMPONENTS PostgreSQL) # 远端 entity 库
find_package(jsoncpp CONFIG REQUIRED)         # 配置 / REST 载荷
find_package(OpenSSL REQUIRED)                # 流式 SHA-256 + TLS 后端
find_package(Threads REQUIRED)
find_package(GTest CONFIG REQUIRED)           # gtest + gmock（测试目标）

# ----------------------------------------------------------------------------- target
add_library(storage_bridge_core STATIC
  src/util.cpp
  src/result.cpp
  src/job_worker.cpp
  src/scan_job.cpp
  src/hasher.cpp
  src/storage.cpp
  src/webdav.cpp
  src/config.cpp
  src/pg_store.cpp
  src/web/app_context.cpp
  src/web/db_pool.cpp
  src/web/entity_lookup.cpp
  src/web/json_util.cpp
  src/web/storage_target.cpp)
target_include_directories(storage_bridge_core PUBLIC src)
target_link_libraries(storage_bridge_core PUBLIC
  Drogon::Drogon
  CURL::libcurl
  SOCI::SOCI
  JsonCpp::JsonCpp
  OpenSSL::Crypto
  OpenSSL::SSL
  Threads::Threads)
target_compile_options(storage_bridge_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

# 控制器靠 Drogon 反射做静态注册，把 *.cpp 编进可执行文件（而不是静态库），
# 避免链接器把“没有外部引用的”翻译单元丢掉。
file(GLOB SB_CONTROLLER_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/web/controllers/*.cpp)

add_executable(storage-bridge
  src/main.cpp
  src/web/bootstrap.cpp
  ${SB_CONTROLLER_SOURCES})
target_link_libraries(storage-bridge PRIVATE storage_bridge_core)
target_compile_options(storage-bridge PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(CLANG_FORMAT_EXE)
  file(GLOB_RECURSE SB_FORMAT_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT_EXE} -i ${SB_FORMAT_SOURCES}
    COMMENT "clang-format -i src/*.{cpp,hpp}"
    VERBATIM)
else()
  message(WARNING "clang-format not found: 跳过编译前格式化（apt-get install clang-format）")
endif()

install(TARGETS storage-bridge RUNTIME DESTINATION bin)

# ----------------------------------------------------------------------------- tests
option(SB_BUILD_TESTS "构建 gtest/gmock 测试" ON)
if(SB_BUILD_TESTS)
  enable_testing()
  include(GoogleTest)
  add_subdirectory(test)
endif()

message(STATUS "")
message(STATUS "storage-bridge ${PROJECT_VERSION} (vcpkg manifest)")
message(STATUS "  drogon     : ${Drogon_VERSION}")
message(STATUS "  curl       : ${CURL_VERSION_STRING}")
message(STATUS "  soci       : ${SOCI_VERSION}")
message(STATUS "  jsoncpp    : ${jsoncpp_VERSION}")
message(STATUS "  openssl    : ${OPENSSL_VERSION}")
message(STATUS "  gtest      : ${GTest_VERSION}")
message(STATUS "  clang-fmt  : ${CLANG_FORMAT_EXE} (手动 target: format)")
message(STATUS "  tests      : ${SB_BUILD_TESTS}")
message(STATUS "")