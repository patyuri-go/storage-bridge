cmake_minimum_required(VERSION 3.16)
project(storage-bridge
  VERSION 0.1.0
  DESCRIPTION "Bridge between object storage (WebDAV/S3) and a remote PostgreSQL catalog"
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
# LazyVim 保存时用 clangd/clang-format 自动格式化；这里在每次编译前也跑一遍，
# 保证“编辑器格式 == 编译产物格式”，避免来回抖动。
option(SB_FORMAT_ON_BUILD "运行 clang-format 后再编译（需要 clang-format）" ON)
find_program(CLANG_FORMAT_EXE NAMES clang-format clang-format-18 clang-format-17)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ----------------------------------------------------------------------------- dependencies
# 全部通过系统开发包发现，不使用任何 FetchContent / 源码内嵌。
# apt: davix-dev libsoci-dev libsoci-postgresql4.0 libpq-dev libboost-dev pkg-config

find_package(PkgConfig REQUIRED)
pkg_check_modules(DAVIX REQUIRED IMPORTED_TARGET davix)

# HTTP 层（Web 前端后端）：cpp-httplib 只提供 pkg-config；nlohmann_json 有 CMake config
pkg_check_modules(HTTPLIB REQUIRED IMPORTED_TARGET cpp-httplib)
find_package(nlohmann_json 3.11 REQUIRED)

# 统一走 SOCI：postgresql = 远端 entity 库（后端仍需系统 libpq）
find_package(SOCI REQUIRED COMPONENTS postgresql)
find_package(PostgreSQL REQUIRED) # 只用于 soci/postgresql 需要的 libpq-fe.h，不直接调用 libpq
find_package(OpenSSL REQUIRED)    # 流式 SHA-256 + WebDAV TLS
find_package(Threads REQUIRED)

# 自写 WebDAV 客户端用 standalone asio（header-only，apt: libasio-dev）
find_path(ASIO_INCLUDE_DIR NAMES asio.hpp REQUIRED)

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
  src/web/server.cpp)
target_include_directories(storage_bridge_core PUBLIC src)
target_include_directories(storage_bridge_core PRIVATE ${PostgreSQL_INCLUDE_DIRS} ${ASIO_INCLUDE_DIR})
target_link_libraries(storage_bridge_core PUBLIC
  PkgConfig::DAVIX
  SOCI::Core SOCI::PostgreSQL
  OpenSSL::Crypto
  OpenSSL::SSL
  PkgConfig::HTTPLIB
  nlohmann_json::nlohmann_json
  Threads::Threads)
target_compile_options(storage_bridge_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(storage_bridge_core PUBLIC SB_DAVIX_VERSION="${DAVIX_VERSION}")

add_executable(storage-bridge src/main.cpp)
target_link_libraries(storage-bridge PRIVATE storage_bridge_core)

if(CLANG_FORMAT_EXE)
  file(GLOB_RECURSE SB_FORMAT_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT_EXE} -i ${SB_FORMAT_SOURCES}
    COMMENT "clang-format -i src/*.{cpp,hpp}"
    VERBATIM)
  if(SB_FORMAT_ON_BUILD)
    # 无输出文件的自定义 target 每次构建都会执行；作为 core 的依赖 → 先格式化再编译
    add_custom_target(format_before_build
      COMMAND ${CLANG_FORMAT_EXE} -i ${SB_FORMAT_SOURCES}
      COMMENT "clang-format (pre-build)"
      VERBATIM)
    add_dependencies(storage_bridge_core format_before_build)
    add_dependencies(storage-bridge format_before_build)
  endif()
else()
  message(WARNING "clang-format not found: 跳过编译前格式化（apt-get install clang-format）")
endif()

install(TARGETS storage-bridge RUNTIME DESTINATION bin)

message(STATUS "")
message(STATUS "storage-bridge ${PROJECT_VERSION}")
message(STATUS "  davix      : ${DAVIX_VERSION} (${DAVIX_LIBRARIES})")
message(STATUS "  SOCI       : ${SOCI_INCLUDE_DIR}")
message(STATUS "  openssl    : ${OPENSSL_VERSION}")
message(STATUS "  asio       : ${ASIO_INCLUDE_DIR} (自带 WebDAV 客户端)")
message(STATUS "  httplib    : ${HTTPLIB_VERSION} (${HTTPLIB_LIBRARIES})")
message(STATUS "  postgres   : ${PostgreSQL_VERSION_STRING} (${PostgreSQL_LIBRARIES})")
message(STATUS "  nlohmann   : (find_package nlohmann_json)")
message(STATUS "  clang-fmt  : ${CLANG_FORMAT_EXE} (format-on-build=${SB_FORMAT_ON_BUILD})")
message(STATUS "")
