#include "scan_job.hpp"

#include "hasher.hpp"
#include "job_worker.hpp"
#include "util.hpp"

#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace sb {

namespace {

constexpr std::size_t kReadChunk = 256 * 1024;

// 把 key 按 '/' 切成非空段（key 形如 /a/b/c.flac）
std::vector<std::string> key_segments(const std::string& key) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < key.size()) {
    while (i < key.size() && key[i] == '/') {
      ++i;
    }
    const std::size_t begin = i;
    while (i < key.size() && key[i] != '/') {
      ++i;
    }
    if (i > begin) {
      out.push_back(key.substr(begin, i - begin));
    }
  }
  return out;
}

} // namespace

const char* to_string(ScanMode mode) {
  switch (mode) {
  case ScanMode::Verify:
    return "verify";
  case ScanMode::Full:
  default:
    return "full";
  }
}

std::string two_level_key(const std::string& key) {
  const std::vector<std::string> segs = key_segments(key);
  if (segs.empty()) {
    return {};
  }
  if (segs.size() == 1) {
    return segs[0];
  }
  return cat(segs[segs.size() - 2], "/", segs.back());
}

std::string join_key(const std::string& dir, const std::string& name) {
  if (name.empty()) {
    return dir;
  }
  if (dir.empty() || dir == "/") {
    return cat("/", name);
  }
  return cat(dir, "/", name);
}

// ---------------------------------------------------------------------------
// ScanFileJob

ScanFileJob::ScanFileJob(std::shared_ptr<const ScanContext> ctx, ObjectRef ref, std::string key)
    : ctx_(std::move(ctx)), ref_(std::move(ref)), key_(std::move(key)) {}

std::string ScanFileJob::label() const {
  return cat("scan-file ", redact_uri(ref_.uri));
}

Status ScanFileJob::run(JobWorker&) {
  if (!ctx_ || !ctx_->storage) {
    return err(ErrorCode::Internal, "扫描任务缺少 storage", "", label());
  }
  if (ctx_->pg == nullptr) {
    return err(ErrorCode::Internal, "扫描任务缺少 PgStore", "", label());
  }
  if (key_.empty()) {
    return err(ErrorCode::Internal, "扫描任务缺少 key", "", label());
  }

  // 校验模式：先按「上级目录/文件名」查已有位置，大小一致就直接认定已扫描。
  // 不读内容、不写库；查不到（或大小不一致）就回退成完整扫描。
  if (ctx_->mode == ScanMode::Verify) {
    std::uint64_t size = ref_.size;
    if (size == 0) {
      // files 数组里显式给的文件没有 size（list 出来的才有），补一次 stat
      auto info = ctx_->storage->stat(ref_.uri);
      if (info && !info->is_dir) {
        size = info->size;
      }
    }
    const std::string probe = two_level_key(key_);
    if (!probe.empty() && size > 0) {
      auto matched =
          ctx_->pg->location_matches_size(ctx_->storage_id, probe, static_cast<long long>(size));
      if (!matched) {
        return matched.error();
      }
      if (*matched) {
        log(LogLevel::Debug, cat("job scan-file verify 命中，跳过: ", probe,
                                 " size=", std::to_string(size), " (", redact_uri(ref_.uri), ")"));
        return {};
      }
    }
  }

  auto reader = Reader::open(*ctx_->storage, ref_.uri);
  if (!reader) {
    return reader.error();
  }

  // 读完整内容算哈希，同时统计真实字节数（entities.size 用它）
  Sha256 hasher;
  std::vector<char> buf(kReadChunk);
  std::uint64_t total = 0;
  std::int64_t offset = 0;
  for (;;) {
    auto got = (*reader)->read_at(offset, buf.data(), buf.size());
    if (!got) {
      return got.error();
    }
    if (*got == 0) {
      break;
    }
    if (Status st = hasher.update(buf.data(), *got); !st) {
      return st;
    }
    offset += static_cast<std::int64_t>(*got);
    total += static_cast<std::uint64_t>(*got);
  }

  auto hex = hasher.hex_final();
  if (!hex) {
    return hex.error();
  }

  auto outcome = ctx_->pg->record_file(ctx_->storage_id, key_, *hex, static_cast<long long>(total),
                                       mime_type_for(key_));
  if (!outcome) {
    log(LogLevel::Warn, cat("job scan-file 添加失败: ", key_, " -> ", outcome.error().to_string()));
  }

  log(LogLevel::Debug,
      cat("job scan-file ", redact_uri(ref_.uri), " sha256=", *hex, " size=", std::to_string(total),
          " -> ", to_string(outcome ? outcome->action : LocationAction::Skipped)));
  return {};
}

// ---------------------------------------------------------------------------
// ScanFolderJob

ScanFolderJob::ScanFolderJob(std::shared_ptr<const ScanContext> ctx, std::string folder_uri,
                             std::string folder_key)
    : ctx_(std::move(ctx)), uri_(normalize_uri(folder_uri)), key_(std::move(folder_key)) {}

std::string ScanFolderJob::label() const {
  return cat("scan-folder ", redact_uri(uri_));
}

Status ScanFolderJob::run(JobWorker& worker) {
  yield_ = false;
  if (!ctx_ || !ctx_->storage) {
    return err(ErrorCode::Internal, "扫描任务缺少 storage", "", label());
  }

  // 第一步：只列一次目录，结果留在 pending_
  if (!listed_) {
    pending_.clear();
    cursor_ = 0;
    Status listed = ctx_->storage->list(uri_, [this](const ObjectRef& ref) -> Status {
      if (!ref.name.empty()) {
        pending_.push_back(ref);
      }
      return {};
    });
    if (!listed) {
      return listed;
    }
    listed_ = true;
    if (pending_.empty()) {
      log(LogLevel::Debug, cat("job scan-folder ", redact_uri(uri_), " -> 0 files, 0 dirs"));
    }
  }

  if (cursor_ >= pending_.size()) {
    log(LogLevel::Debug, cat("job scan-folder ", redact_uri(uri_), " -> ", std::to_string(files_),
                             " files, ", std::to_string(dirs_), " dirs"));
    return {};
  }

  // 第二步：一次只提交一个子任务
  const ObjectRef ref = pending_[cursor_];
  const std::string child_key = join_key(key_, ref.name);
  Result<std::unique_ptr<Job>> child = ref.is_dir ? make_scan_folder_job(ctx_, ref.uri, child_key)
                                                  : make_scan_file_job(ctx_, ref.uri, child_key);
  if (!child) {
    // 单个子项建不出来（基本不会发生）就跳过它，别让整个目录失败
    log(LogLevel::Warn,
        cat("job scan-folder 跳过子项 ", child_key, ": ", child.error().to_string()));
    ++cursor_;
    if (cursor_ < pending_.size()) {
      yield_ = true;
    }
    return {};
  }

  if (Status st = worker.spawn(std::move(*child)); !st) {
    if (st.error().code == ErrorCode::Busy) {
      // 队列满：保存现场，等队列消化掉再来。稍等一会儿，避免几个目录任务空转。
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      yield_ = true;
      return {};
    }
    return st;
  }

  ++cursor_;
  if (ref.is_dir) {
    ++dirs_;
  } else {
    ++files_;
  }

  // 让出执行权：文件任务优先，下次再回来接着提交
  if (cursor_ < pending_.size()) {
    yield_ = true;
  }
  return {};
}

// ---------------------------------------------------------------------------
// 入口

Result<std::unique_ptr<Job>> make_scan_folder_job(std::shared_ptr<const ScanContext> ctx,
                                                  std::string folder_uri, std::string folder_key) {
  if (!ctx || !ctx->storage) {
    return err(ErrorCode::InvalidArgument, "ScanContext.storage 不能为空");
  }
  std::string uri = trim(std::move(folder_uri));
  if (uri.empty()) {
    return err(ErrorCode::InvalidArgument, "folder_uri 不能为空");
  }
  if (uri.find("://") == std::string::npos) {
    return err(ErrorCode::InvalidArgument, "folder_uri 需要完整 URL（含 scheme）", "", uri);
  }
  std::string key = trim(std::move(folder_key));
  if (key.empty()) {
    return err(ErrorCode::InvalidArgument, "folder_key 不能为空");
  }
  return std::unique_ptr<Job>(new ScanFolderJob(std::move(ctx), std::move(uri), std::move(key)));
}

Result<std::unique_ptr<Job>> make_scan_file_job(std::shared_ptr<const ScanContext> ctx,
                                                std::string file_uri, std::string file_key) {
  if (!ctx || !ctx->storage) {
    return err(ErrorCode::InvalidArgument, "ScanContext.storage 不能为空");
  }
  std::string uri = trim(std::move(file_uri));
  if (uri.empty()) {
    return err(ErrorCode::InvalidArgument, "file_uri 不能为空");
  }
  if (uri.find("://") == std::string::npos) {
    return err(ErrorCode::InvalidArgument, "file_uri 需要完整 URL（含 scheme）", "", uri);
  }

  ObjectRef ref;
  ref.uri = normalize_uri(uri);
  ref.name = basename_of(uri);
  ref.is_dir = false;

  std::string key = trim(std::move(file_key));
  if (key.empty()) {
    key = cat("/", ref.name);
  }
  return std::unique_ptr<Job>(new ScanFileJob(std::move(ctx), std::move(ref), std::move(key)));
}

} // namespace sb
