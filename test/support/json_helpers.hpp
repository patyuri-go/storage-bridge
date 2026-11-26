#pragma once

#include <json/json.h>

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace sb::test {

inline Json::Value parse_json(const std::string& text) {
  Json::CharReaderBuilder builder;
  Json::Value out;
  std::string errors;
  std::istringstream in(text);
  const bool ok = Json::parseFromStream(builder, in, &out, &errors);
  EXPECT_TRUE(ok) << "JSON 解析失败: " << errors << "\n原始内容: " << text;
  return out;
}

} // namespace sb::test