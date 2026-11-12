#pragma once

#include "job.hpp"
#include "pg_store.hpp"
#include "storage.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sb {

// 扫描模式
//   Full   —— 读完整内容算 SHA-256，然后写库（默认）
//   Verify —— 先按「上级目录/文件名」查 entity_locations，有大小一致的实体就认定
//             已扫描、直接跳过（不读内容、不写库）；查不到才回退成 Full
enum class ScanMode { Full, Verify };

const char* to_string(ScanMode mode);

// 一次扫描任务的共享上下文。storage 由 context 持有，所有子任务复用。
struct ScanContext {
  std::shared_ptr<Storage> storage;
  std::string storage_id;
  PgStore* pg = nullptr; // 哈希模式的写入端，不能为空
  ScanMode mode = ScanMode::Full;
};

// 拼接 key：join_key("/a/b", "c.flac") -> "/a/b/c.flac"，join_key("/", "x") -> "/x"
std::string join_key(const std::string& dir, const std::string& name);

// 取 key 的最后两级：「上级目录/文件名」。
//   /a/b/c.flac -> b/c.flac        /c.flac -> c.flac        / -> ""
// 校验模式用它去 entity_locations 里模糊匹配（表里存的是完整路径）。
std::string two_level_key(const std::string& key);

// 扫描单个文件（哈希模式）：读完整内容算 SHA-256，然后按 entities /
// entity_locations 的口径写库。
class ScanFileJob : public Job {
public:
  ScanFileJob(std::shared_ptr<const ScanContext> ctx, ObjectRef ref, std::string key);

  JobKind kind() const override {
    return JobKind::ScanFile;
  }
  std::string label() const override;
  Status run(JobWorker& worker) override;

private:
  std::shared_ptr<const ScanContext> ctx_;
  ObjectRef ref_;
  std::string key_;
};

// 扫描一个目录：列一层，文件入队 ScanFileJob，子目录入队 ScanFolderJob。
//
// 这是个可恢复（协作式）任务：列目录只做一次，结果存进 pending_，之后每被调度
// 一次就只提交一个子任务然后把 yielded() 置位让出执行权。这样展开大目录不会
// 长时间占住 worker，文件扫描任务能立刻插到队首优先跑完；队列满时保留现场，
// 下次调度再试同一项。
class ScanFolderJob : public Job {
public:
  // folder_uri 需要完整 URL（例如 http://host:5005/dav/a/），内部会 normalize
  ScanFolderJob(std::shared_ptr<const ScanContext> ctx, std::string folder_uri,
                std::string folder_key);

  JobKind kind() const override {
    return JobKind::ScanFolder;
  }
  std::string label() const override;
  Status run(JobWorker& worker) override;
  bool yielded() const override {
    return yield_;
  }

private:
  std::shared_ptr<const ScanContext> ctx_;
  std::string uri_;
  std::string key_;

  bool listed_ = false;            // 是否已经列过目录
  std::vector<ObjectRef> pending_; // 列出来的子项，已过滤空 name
  std::size_t cursor_ = 0;         // 下一个要提交的下标
  std::uint64_t files_ = 0;        // 已提交的文件子任务数
  std::uint64_t dirs_ = 0;         // 已提交的目录子任务数
  bool yield_ = false;             // 本轮的"请让我先让出"标记
};

// 入口：校验并创建根扫描任务
Result<std::unique_ptr<Job>> make_scan_folder_job(std::shared_ptr<const ScanContext> ctx,
                                                  std::string folder_uri, std::string folder_key);

// 入口：为一个已经确定的文件创建任务（key 为空时按 uri 最后一段兜底）
Result<std::unique_ptr<Job>> make_scan_file_job(std::shared_ptr<const ScanContext> ctx,
                                                std::string file_uri, std::string file_key);

} // namespace sb
