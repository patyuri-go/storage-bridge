#include "storage.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace sb {

namespace {

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

Result<std::unique_ptr<Storage>> Storage::create(const StorageOptions& opts,
                                                  std::shared_ptr<webdav::IWebDavClient> client) {
  std::unique_ptr<Storage> self(new Storage());
  self->opts_ = opts;
  self->client_ = client ? std::move(client) : std::make_shared<webdav::CurlWebDavClient>();
  return self;
}

Storage::~Storage() = default;

webdav::ClientOptions Storage::webdav_options() const {
  webdav::ClientOptions out;
  out.timeout = opts_.timeout > 0 ? opts_.timeout : 60;
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
  auto url = webdav_base(uri);
  if (!url) {
    return url.error();
  }

  const webdav::Response resp = client_->propfind(*url, url->path, 0, webdav_options());
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
  auto url = webdav_base(base);
  if (!url) {
    return url.error();
  }

  std::string path = url->path;
  if (path.empty() || path.back() != '/') {
    path.push_back('/');
  }

  const webdav::Response resp = client_->propfind(*url, path, 1, webdav_options());
  if (resp.status != 207) {
    return err(ErrorCode::Storage, "列目录失败", cat("HTTP ", std::to_string(resp.status)),
               redact_uri(base));
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

  auto url = storage.webdav_base(uri);
  if (!url) {
    return url.error();
  }
  self->base_ = *url;
  self->path_ = url->path;
  self->opts_ = storage.webdav_options();
  self->client_ = storage.client_;
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

Reader::~Reader() = default;

Status Reader::open_stream(std::int64_t offset) {
  stream_.reset();
  skip_ = 0;
  pos_ = offset;

  std::vector<std::string> extra;
  if (offset > 0) {
    extra.push_back(cat("Range: bytes=", std::to_string(offset), "-"));
  }

  stream_ = client_->open_stream(base_, path_, extra, opts_);
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

} // namespace sb