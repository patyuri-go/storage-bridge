#pragma once

#include "support/mock_database.hpp"
#include "support/test_runtime.hpp"

#include <gtest/gtest.h>

namespace sb::test {

// 集成用例统一基类：每个用例开始/结束都重置 gMock 期望，服务本身只启动一次。
class IntegrationTest : public ::testing::Test {
protected:
  void SetUp() override { TestRuntime::instance().reset_mocks(); }
  void TearDown() override { TestRuntime::instance().reset_mocks(); }

  static TestRuntime& runtime() { return TestRuntime::instance(); }
};

} // namespace sb::test