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
# LazyVim 保存时用 clangd/clang-format 自动格式化；这里在每次编译前也跑一遍，
# 保证“编辑器格式 == 编译产物格式”，避免来回抖动。
option(SB_FORMAT_ON_BUILD "运行 clang-format 后再编译（需要 clang-format）" ON)
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
message(STATUS "storage-bridge ${PROJECT_VERSION} (vcpkg manifest)")
message(STATUS "  drogon     : ${Drogon_VERSION}")
message(STATUS "  curl       : ${CURL_VERSION_STRING}")
message(STATUS "  soci       : ${SOCI_VERSION}")
message(STATUS "  jsoncpp    : ${jsoncpp_VERSION}")
message(STATUS "  openssl    : ${OPENSSL_VERSION}")
message(STATUS "  clang-fmt  : ${CLANG_FORMAT_EXE} (format-on-build=${SB_FORMAT_ON_BUILD})")
message(STATUS "")