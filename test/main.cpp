#include "support/test_runtime.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--gtest_list_tests") == 0) {
      list_only = true;
      break;
    }
  }

  ::testing::InitGoogleTest(&argc, argv);

  // gtest_discover_tests 会用 --gtest_list_tests 启动本二进制；
  // 此时不能起服务，否则每个测试都要多等一次监听/健康检查。
  if (list_only) {
    return RUN_ALL_TESTS();
  }

  sb::test::TestRuntime& runtime = sb::test::TestRuntime::instance();
  if (sb::Status st = runtime.start(); !st) {
    std::cerr << "测试服务启动失败: " << st.error().to_string() << std::endl;
    return 2;
  }

  const int rc = RUN_ALL_TESTS();
  runtime.stop();
  return rc;
}