#include "storage.hpp"

#include <auth/davixx509cred.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace sb {

namespace {

// 把 davix 的错误对象转成 Error 并清理
Error davix_error(ErrorCode code, const std::string& what, Davix::DavixError** davix_err,
                  const std::string& context = {}) {
  std::string detail;
  if (davix_err != nullptr && *davix_err != nullptr) {
    detail = (*davix_err)->getErrMsg();
    Davix::DavixError::clearError(davix_err);
  }
  return sb::err(code, what, detail, context);
}

// "http://h/a/b/" -> "b"（输入保持百分号编码，返回仍编码，调用方决定是否解码）
std::string last_segment(const std::string& path) {
  std::string s = path;
  while (!s.empty() && s.back() == '/') {
    s.pop_back();
  }
  const auto slash = s.find_last_of('/');
  return (slash == std::string::npos) ? s : s.substr(slash + 1);
}

std::string without_trailing_slash(const std::string& path) {
  std::string s = path;
  while (s.size() > 1 && s.back() == '/') {
    s.pop_back();
  }
  return s;
}

// 判断 PROPFIND 返回的 href 是不是「请求的那个目录/文件自己」。
// 我们发出的 path 是原始字符（可能含空格、中文、方括号），服务器回的 href
// 一律百分号编码，所以必须先都解码再比。
bool same_path(const std::string& lhs, const std::string& rhs) {
  return webdav::percentDecode(without_trailing_slash(lhs)) ==
         webdav::percentDecode(without_trailing_slash(rhs));
}

} // namespace

Result<std::unique_ptr<Storage>> Storage::create(const StorageOptions& opts) {
  std::unique_ptr<Storage> self(new Storage());
  self->opts_ = opts;

  self->params_.setUserAgent(opts.user_agent);
  self->params_.setSSLCAcheck(opts.ssl_verify);
  self->params_.setOperationRetry(opts.retries);
  self->params_.setOperationRetryDelay(opts.retry_delay);

  timespec connect_ts = make_timespec(opts.connect_timeout);
  timespec op_ts = make_timespec(opts.operation_timeout);
  self->params_.setConnectionTimeout(&connect_ts);
  self->params_.setOperationTimeout(&op_ts);

  // 只在 davix 走 neon 后端时生效；系统包是 libcurl 后端，这个调用是空操作。
  // WebDAV 现在由 src/webdav.cpp 处理，预发 Authorization 头由那边负责。
  if (!opts.user.empty() || !opts.password.empty()) {
    self->params_.setClientLoginPassword(opts.user, opts.password);
  }
  if (!opts.bearer_token.empty()) {
    self->params_.addHeader("Authorization", "Bearer " + opts.bearer_token);
  }
  if (!opts.cert.empty() || !opts.cert_key.empty()) {
    Davix::X509Credential cred;
    Davix::DavixError* davix_err = nullptr;
    const std::string key = opts.cert_key.empty() ? opts.cert : opts.cert_key;
    const std::string cert = opts.cert.empty() ? opts.cert_key : opts.cert;
    if (cred.loadFromFilePEM(key, cert, opts.cert_password, &davix_err) != 0) {
      return davix_error(ErrorCode::Config, "无法加载 x509 证书", &davix_err,
                         opts.cert.empty() ? opts.cert_key : opts.cert);
    }
    self->params_.setClientCertX509(cred);
  }
  if (!opts.s3_access_key.empty() || !opts.s3_secret_key.empty()) {
    // 注意 davix 的参数顺序是 (secret, access)
    self->params_.setAwsAuthorizationKeys(opts.s3_secret_key, opts.s3_access_key);
  }
  if (!opts.s3_region.empty()) {
    self->params_.setAwsRegion(opts.s3_region);
  }
  if (!opts.s3_token.empty()) {
    self->params_.setAwsToken(opts.s3_token);
  }
  return self;
}

Storage::~Storage() = default;

bool Storage::is_webdav(const std::string& uri) {
  return webdav::isWebdavUrl(uri);
}

webdav::ClientOptions Storage::webdav_options() const {
  webdav::ClientOptions out;
  out.timeout = opts_.operation_timeout > 0 ? opts_.operation_timeout : 30;
  out.ssl_verify = opts_.ssl_verify;
  out.user_agent = opts_.user_agent;
  return out;
}

Result<webdav::Url> Storage::webdav_base(const std::string& uri) const {
  webdav::Url url;
  if (!webdav::parseUrl(normalize_uri(uri), url)) {
    return err(ErrorCode::InvalidArgument, "不是合法的 http(s) URL", "", redact_uri(uri));
  }
  // URI 自带 userinfo 时优先用它的，否则用 StorageOptions 里的凭据
  if (url.user.empty() && url.pass.empty()) {
    url.user = opts_.user;
    url.pass = opts_.password;
  }
  if (url.path.empty()) {
    url.path = "/";
  }
  return url;
}

Result<ObjectRef> Storage::stat(const std::string& uri) {
  if (is_webdav(uri)) {
    return stat_webdav(uri);
  }

  ObjectRef ref;
  ref.uri = normalize_uri(uri);
  ref.name = basename_of(ref.uri);

  try {
    Davix::File file(ctx_, Davix::Uri(ref.uri));
    Davix::StatInfo info;
    file.statInfo(&params_, info);
    ref.size = info.size;
    ref.mtime = info.mtime;
    ref.is_dir = ((info.mode & S_IFMT) == S_IFDIR);
  } catch (const Davix::DavixException& e) {
    return err(ErrorCode::Storage, "stat 失败", e.what(), redact_uri(ref.uri));
  }
  return ref;
}

Result<ObjectRef> Storage::stat_webdav(const std::string& uri) {
  auto url = webdav_base(uri);
  if (!url) {
    return url.error();
  }

  webdav::Client client(*url, webdav_options());
  const webdav::Response resp = client.propfind(url->path, 0);
  if (resp.status != 207) {
    return err(ErrorCode::Storage, "stat 失败", cat("HTTP ", std::to_string(resp.status)),
               redact_uri(uri));
  }

  const std::vector<webdav::Resource> items = webdav::parsePropfind(resp.body);
  if (items.empty()) {
    return err(ErrorCode::Storage, "stat 失败", "PROPFIND 没有返回任何 response 元素",
               redact_uri(uri));
  }

  const webdav::Resource* hit = &items.front();
  for (const auto& item : items) {
    if (same_path(item.path, url->path)) {
      hit = &item;
      break;
    }
  }

  ObjectRef ref;
  ref.uri = normalize_uri(uri);
  ref.name = webdav::percentDecode(last_segment(hit->path));
  ref.size = hit->size < 0 ? 0 : static_cast<std::uint64_t>(hit->size);
  ref.mtime = hit->mtime;
  ref.is_dir = hit->collection;
  return ref;
}

Status Storage::list(const std::string& base, const std::function<Status(const ObjectRef&)>& cb) {
  if (is_webdav(base)) {
    return list_webdav(base, cb);
  }

  const std::string root = normalize_uri(base);
  try {
    Davix::File dir(ctx_, Davix::Uri(root));
    Davix::File::Iterator it = dir.listCollection(&params_);

    auto emit = [&](const std::string& name, const Davix::StatInfo& info) -> Status {
      ObjectRef ref;
      ref.name = name;
      ref.uri = join_uri(root, name);
      ref.size = info.size;
      ref.mtime = info.mtime;
      ref.is_dir = ((info.mode & S_IFMT) == S_IFDIR);
      return cb(ref);
    };

    // 上游 bug 规避（davix 0.8.5 / 0.9.0 的 legacy iterator）：
    // DavFile::Iterator::Internal 构造时就调用了一次 nextSubItem() 把第一项
    // prime 进 (name, info)，而 Iterator::next() 是"先前进再返回"，于是每个目录的
    // 第一个子项被静默丢弃 —— 目录里只有一个对象时会直接返回 0 条。
    // 这里手工取回 primed 项。详见 docs/feasibility.md「davix 上游 bug」。
    if (!it.name().empty()) {
      if (Status st = emit(it.name(), it.info()); !st) {
        return st;
      }
    }
    while (it.next()) {
      if (Status st = emit(it.name(), it.info()); !st) {
        return st;
      }
    }
  } catch (const Davix::DavixException& e) {
    return err(ErrorCode::Storage, "列目录失败", e.what(), redact_uri(root));
  }
  return {};
}

Status Storage::list_webdav(const std::string& root,
                            const std::function<Status(const ObjectRef&)>& cb) {
  auto url = webdav_base(root);
  if (!url) {
    return url.error();
  }

  std::string path = url->path;
  if (path.back() != '/') {
    path.push_back('/');
  }

  webdav::Client client(*url, webdav_options());
  const webdav::Response resp = client.propfind(path, 1);
  if (resp.status != 207) {
    return err(ErrorCode::Storage, "列目录失败", cat("HTTP ", std::to_string(resp.status)),
               redact_uri(root));
  }

  const std::vector<webdav::Resource> items = webdav::parsePropfind(resp.body);

  for (const auto& item : items) {
    if (same_path(item.path, path)) {
      continue; // Depth:1 的第一项是目录自身
    }
    ObjectRef ref;
    ref.uri = normalize_uri(webdav::buildUrl(*url, item.path));
    ref.name = webdav::percentDecode(last_segment(item.path));
    ref.size = item.size < 0 ? 0 : static_cast<std::uint64_t>(item.size);
    ref.mtime = item.mtime;
    ref.is_dir = item.collection;
    if (Status st = cb(ref); !st) {
      return st;
    }
  }
  return {};
}

Result<std::unique_ptr<Reader>> Reader::open(Storage& storage, const std::string& uri) {
  std::unique_ptr<Reader> self(new Reader());
  self->storage_ = &storage;

  if (Storage::is_webdav(uri)) {
    auto url = storage.webdav_base(uri);
    if (!url) {
      return url.error();
    }
    self->webdav_ = true;
    self->base_ = *url;
    self->path_ = url->path;
    self->opts_ = storage.webdav_options();
    self->uri_ = normalize_uri(uri);

    auto info = storage.stat(uri);
    if (!info) {
      return info.error();
    }
    if (info->is_dir) {
      // 目录不能按文件读：否则服务器会返回目录 HTML / listing，被当成内容算哈希
      return err(ErrorCode::InvalidArgument, "目标是目录，不能按文件读取", "", redact_uri(uri));
    }
    return self;
  }

  try {
    self->file_ = std::make_unique<Davix::File>(storage.context(), Davix::Uri(normalize_uri(uri)));
    Davix::StatInfo info;
    self->file_->statInfo(&storage.params(), info);
  } catch (const Davix::DavixException& e) {
    return err(ErrorCode::Storage, "打开对象失败", e.what(), redact_uri(uri));
  }
  return self;
}

Reader::~Reader() = default;

Status Reader::open_stream(std::int64_t offset) {
  stream_.reset();
  skip_ = 0;
  pos_ = offset;

  std::vector<std::string> extra;
  if (offset > 0) {
    extra.push_back(cat("Range: bytes=", std::to_string(offset), "-"));
  }

  stream_ = webdav::openStream(base_, path_, extra, opts_);
  if (stream_.status == 0) {
    return err(ErrorCode::Network, "下载请求失败（无法连接或不完整响应）", "", redact_uri(uri_));
  }
  if (stream_.status != 200 && stream_.status != 206) {
    return err(ErrorCode::Storage, "下载失败", cat("HTTP ", std::to_string(stream_.status)),
               redact_uri(uri_));
  }
  if (stream_.status == 200 && offset > 0) {
    // 服务器忽略了 Range，响应体从 0 开始，先丢掉前 offset 个字节
    skip_ = offset;
    pos_ = 0;
  }
  return {};
}

Result<std::size_t> Reader::read_at(std::int64_t offset, void* buf, std::size_t len) {
  if (len == 0) {
    return 0;
  }
  if (offset < 0) {
    return err(ErrorCode::InvalidArgument, "offset 不能为负", "", std::to_string(offset));
  }

  if (webdav_) {
    if (!stream_.read || offset != pos_) {
      if (Status st = open_stream(offset); !st) {
        return st.error();
      }
    }

    char* out = static_cast<char*>(buf);
    std::size_t total = 0;
    while (total < len) {
      if (skip_ > 0) {
        char scratch[8192];
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::int64_t>(skip_, sizeof(scratch)));
        const long long got = stream_.read(scratch, want);
        if (got <= 0) {
          break;
        }
        skip_ -= got;
        pos_ += got;
        continue;
      }
      const long long got = stream_.read(out + total, len - total);
      if (got < 0) {
        return err(ErrorCode::Storage, "读取失败（连接中断）", "", redact_uri(uri_));
      }
      if (got == 0) {
        break; // EOF
      }
      total += static_cast<std::size_t>(got);
      pos_ += got;
    }
    return total;
  }

  Davix::DavixError* davix_err = nullptr;
  const dav_ssize_t n = file_->readPartial(&storage_->params(), buf, static_cast<dav_size_t>(len),
                                           static_cast<dav_off_t>(offset), &davix_err);
  if (n < 0) {
    return davix_error(ErrorCode::Storage, "读取失败", &davix_err,
                       redact_uri(file_->getUri().getString()));
  }
  return static_cast<std::size_t>(n);
}

} // namespace sb
