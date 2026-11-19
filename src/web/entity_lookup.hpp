#pragma once

#include "web/storage_target.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace sb::web {

// 一次「路径 -> 实体」查找里远端遍历的结果。
struct LookupWalk {
  std::vector<std::string> files;                          // 存储上实际有的文件 rel_path（已排序去重）
  std::vector<std::pair<std::string, std::string>> errors; // (rel_path, message)，已排序、已截断
  std::size_t error_count = 0;                             // errors 截断前的总数
  bool truncated = false;                                  // 10 秒内没遍历完
  bool any_input_ok = false;                               // 至少有一个输入路径 stat 成功
};

// 在 target 上递归遍历 rel_paths：目录递归展开，文件直接算一条。
// 8 个线程并发 PROPFIND，超过 10 秒就带着已收集的部分返回（后台线程自行退出）。
LookupWalk walk_storage_paths(const StorageTarget& target, const std::vector<std::string>& rel_paths);

} // namespace sb::web
