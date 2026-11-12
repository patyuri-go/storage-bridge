# FindSOCI.cmake
#
# Ubuntu/Debian 的 libsoci-dev 既没有 CMake config 也没有 pkg-config 文件，
# 只提供 <soci/*.h> 和 libsoci_{core,postgresql}.so，因此这里手工构造
# imported target：
#   SOCI::Core        (required)
#   SOCI::PostgreSQL  (component postgresql)
#
# 注意：Ubuntu 的 SOCI 4.0.3 是以 WITH_BOOST=ON 编译的，其公有头
# （soci/bind-values.h）无条件 #include <boost/fusion/...>，所以消费方
# 必须能找到 Boost 头文件（libboost-dev）。

include(CheckIncludeFileCXX)

find_path(SOCI_INCLUDE_DIR
  NAMES soci/soci.h
  PATH_SUFFIXES soci include/soci)

find_library(SOCI_CORE_LIB NAMES soci_core libsoci_core)

if(NOT SOCI_INCLUDE_DIR OR NOT SOCI_CORE_LIB)
  set(SOCI_FOUND FALSE)
  if(SOCI_FIND_REQUIRED)
    message(FATAL_ERROR
      "SOCI not found. Install it with: apt-get install libsoci-dev "
      "(looked for soci/soci.h and libsoci_core.so)")
  endif()
  return()
endif()

add_library(SOCI::Core UNKNOWN IMPORTED)
set_target_properties(SOCI::Core PROPERTIES
  IMPORTED_LOCATION "${SOCI_CORE_LIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${SOCI_INCLUDE_DIR}")

# SOCI 的公有头是否依赖 Boost（发行版打包差异，探测而非硬编码）
check_include_file_cxx("boost/fusion/algorithm/iteration/for_each.hpp" SOCI_HAS_BOOST_FUSION)
if(SOCI_HAS_BOOST_FUSION)
  find_package(Boost QUIET)
  if(Boost_FOUND AND Boost_INCLUDE_DIRS)
    target_include_directories(SOCI::Core INTERFACE ${Boost_INCLUDE_DIRS})
  endif()
endif()

# ---- optional components -----------------------------------------------------
set(SOCI_${CMAKE_FIND_PACKAGE_NAME}_FOUND TRUE)

foreach(_comp postgresql)
  string(TOUPPER "${_comp}" _COMP_UPPER)
  list(FIND SOCI_FIND_COMPONENTS "${_comp}" _comp_wanted)
  if(_comp_wanted EQUAL -1)
    continue()
  endif()

  find_library(SOCI_${_COMP_UPPER}_LIB NAMES soci_${_comp} libsoci_${_comp})
  if(NOT SOCI_${_COMP_UPPER}_LIB)
    set(SOCI_${_comp}_FOUND FALSE)
    if(SOCI_FIND_REQUIRED_${_comp})
      message(FATAL_ERROR
        "SOCI component '${_comp}' not found. Install it with: "
        "apt-get install libsoci-${_comp}4.0")
    endif()
    continue()
  endif()

  add_library(SOCI::PostgreSQL UNKNOWN IMPORTED)
  set_target_properties(SOCI::PostgreSQL PROPERTIES
    IMPORTED_LOCATION "${SOCI_${_COMP_UPPER}_LIB}"
    INTERFACE_LINK_LIBRARIES SOCI::Core)
  set(SOCI_${_comp}_FOUND TRUE)
endforeach()

set(SOCI_INCLUDE_DIRS "${SOCI_INCLUDE_DIR}")
set(SOCI_LIBRARIES "${SOCI_CORE_LIB}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SOCI DEFAULT_MSG SOCI_INCLUDE_DIR SOCI_CORE_LIB)

mark_as_advanced(SOCI_INCLUDE_DIR SOCI_CORE_LIB SOCI_POSTGRESQL_LIB)
