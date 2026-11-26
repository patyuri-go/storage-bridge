#include "webdav.hpp"

#include "util.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <utility>

namespace sb::webdav {

namespace {

int hexValue(unsigned char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

std::string xmlUnescape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out += s[i];
      continue;
    }
    const std::size_t semi = s.find(';', i + 1);
    if (semi == std::string::npos || semi - i > 12) {
      out += s[i];
      continue;
    }
    const std::string ent = s.substr(i + 1, semi - i - 1);
    if (ent == "amp") {
      out += '&';
    } else if (ent == "lt") {
      out += '<';
    } else if (ent == "gt") {
      out += '>';
    } else if (ent == "quot") {
      out += '"';
    } else if (ent == "apos") {
      out += '\'';
    } else if (ent.size() > 1 && ent[0] == '#') {
      // 数值实体 → UTF-8
      long code = 0;
      bool ok = true;
      if (ent[1] == 'x' || ent[1] == 'X') {
        if (ent.size() == 2) {
          ok = false;
        }
        for (std::size_t k = 2; ok && k < ent.size(); ++k) {
          const int v = hexValue(static_cast<unsigned char>(ent[k]));
          if (v < 0) {
            ok = false;
          } else {
            code = code * 16 + v;
          }
        }
      } else {
        for (std::size_t k = 1; ok && k < ent.size(); ++k) {
          const char c = ent[k];
          if (c < '0' || c > '9') {
            ok = false;
          } else {
            code = code * 10 + (c - '0');
          }
        }
      }
      if (!ok || code <= 0 || code > 0x10FFFF) {
        out += s.substr(i, semi - i + 1);
      } else if (code < 0x80) {
        out += static_cast<char>(code);
      } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
      } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
      } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
      }
    } else {
      out += s.substr(i, semi - i + 1);
    }
    i = semi;
  }
  return out;
}

// 取 <name>...</name> 的文本；遇到自闭合标签跳过
std::string extractTag(const std::string& block, const std::string& name) {
  std::size_t pos = 0;
  while (true) {
    const std::size_t at = block.find(name, pos);
    if (at == std::string::npos) {
      return {};
    }
    const std::size_t open = block.find('>', at);
    if (open == std::string::npos) {
      return {};
    }
    if (open > 0 && block[open - 1] == '/') {
      pos = open + 1;
      continue;
    }
    const std::size_t close = block.find('<', open + 1);
    if (close == std::string::npos) {
      return {};
    }
    return block.substr(open + 1, close - open - 1);
  }
}

void ensureCurlGlobal() {
  static const bool initialized = [] {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return true;
  }();
  (void)initialized;
}

// 空格必须编码，否则 libcurl 会直接拒绝该 URL
std::string encodeSpaces(const std::string& path) {
  std::string out;
  out.reserve(path.size());
  for (const char c : path) {
    if (c == ' ') {
      out += "%20";
    } else {
      out += c;
    }
  }
  return out;
}

void applyCommonOptions(CURL* handle, const Url& target, const ClientOptions& opts,
                        const std::string& url) {
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(handle, CURLOPT_USERAGENT, opts.user_agent.c_str());
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, static_cast<long>(opts.timeout));
  // 用低速率超时代替 socket 级 recv/send 超时：整块传输可以很慢，
  // 但连续 timeout 秒没有任何进展就判定失败。
  curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, static_cast<long>(opts.timeout));
  curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, opts.ssl_verify ? 1L : 0L);
  curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, opts.ssl_verify ? 2L : 0L);
  if (!target.user.empty() || !target.pass.empty()) {
    curl_easy_setopt(handle, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC));
    curl_easy_setopt(handle, CURLOPT_USERNAME, target.user.c_str());
    curl_easy_setopt(handle, CURLOPT_PASSWORD, target.pass.c_str());
  }
}

// 记录响应头；每见到新的状态行就清空（重定向/100-continue 会多次出现）
bool absorbHeaderLine(std::map<std::string, std::string>& headers, const char* data,
                      std::size_t len) {
  std::string line(data, len);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  if (line.rfind("HTTP/", 0) == 0) {
    headers.clear();
    return false;
  }
  const std::size_t colon = line.find(':');
  if (colon == std::string::npos) {
    return false;
  }
  headers[to_lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
  return true;
}

std::size_t writeToString(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

std::size_t headerToMap(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
  absorbHeaderLine(*headers, ptr, size * nmemb);
  return size * nmemb;
}

} // namespace

bool isWebdavUrl(const std::string& url) {
  const std::string l = to_lower(url);
  return l.rfind("http://", 0) == 0 || l.rfind("https://", 0) == 0;
}

bool parseUrl(const std::string& url, Url& out) {
  const std::size_t p = url.find("://");
  if (p == std::string::npos) {
    return false;
  }
  const std::string scheme = to_lower(url.substr(0, p));
  if (scheme == "https") {
    out.https = true;
  } else if (scheme == "http") {
    out.https = false;
  } else {
    return false;
  }

  const std::string rest = url.substr(p + 3);
  const std::size_t slash = rest.find('/');
  std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  const std::string path = (slash == std::string::npos) ? "/" : rest.substr(slash);

  const std::size_t at = authority.find('@');
  if (at != std::string::npos) {
    const std::string userinfo = authority.substr(0, at);
    authority = authority.substr(at + 1);
    const std::size_t colon = userinfo.find(':');
    if (colon == std::string::npos) {
      out.user = percentDecode(userinfo);
    } else {
      out.user = percentDecode(userinfo.substr(0, colon));
      out.pass = percentDecode(userinfo.substr(colon + 1));
    }
  }

  if (!authority.empty() && authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string::npos) {
      return false;
    }
    out.host = authority.substr(0, close + 1);
    if (close + 1 < authority.size() && authority[close + 1] == ':') {
      out.port = authority.substr(close + 2);
    }
  } else {
    const std::size_t colon = authority.find(':');
    if (colon == std::string::npos) {
      out.host = authority;
    } else {
      out.host = authority.substr(0, colon);
      out.port = authority.substr(colon + 1);
    }
  }

  if (out.host.empty()) {
    return false;
  }
  out.path = path;
  return true;
}

std::string percentDecode(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const int hi = hexValue(static_cast<unsigned char>(text[i + 1]));
      const int lo = hexValue(static_cast<unsigned char>(text[i + 2]));
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

std::string buildUrl(const Url& base, const std::string& encoded_path) {
  std::string out = base.https ? "https://" : "http://";
  out += base.host;
  if (!base.port.empty()) {
    out += ':';
    out += base.port;
  }
  out += encoded_path.empty() ? "/" : encoded_path;
  return out;
}

std::int64_t httpDateToEpoch(const std::string& text) {
  // Wed, 24 Jun 2026 03:17:54 GMT
  static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  std::tm tm{};
  char weekday[8] = {0};
  char month[8] = {0};
  int day = 0;
  int year = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (std::sscanf(text.c_str(), "%3s, %2d %3s %4d %2d:%2d:%2d", weekday, &day, month, &year, &hour,
                  &minute, &second) != 7) {
    return 0;
  }
  int mon = -1;
  for (int i = 0; i < 12; ++i) {
    if (std::strcmp(month, kMonths[i]) == 0) {
      mon = i;
      break;
    }
  }
  if (mon < 0) {
    return 0;
  }
  tm.tm_year = year - 1900;
  tm.tm_mon = mon;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
  const std::time_t t = timegm(&tm);
  return (t < 0) ? 0 : static_cast<std::int64_t>(t);
}

std::string Response::header(const std::string& key) const {
  const auto it = headers.find(to_lower(key));
  return it == headers.end() ? std::string() : it->second;
}

Response Client::request(const std::string& method, const std::string& path,
                         const std::vector<std::string>& extra, const std::string& body) {
  ensureCurlGlobal();

  Response resp;
  CURL* handle = curl_easy_init();
  if (handle == nullptr) {
    return resp;
  }

  applyCommonOptions(handle, base_, opts_, buildUrl(base_, encodeSpaces(path)));
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &resp.body);
  curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, headerToMap);
  curl_easy_setopt(handle, CURLOPT_HEADERDATA, &resp.headers);

  curl_slist* headers = nullptr;
  for (const auto& h : extra) {
    headers = curl_slist_append(headers, h.c_str());
  }
  if (headers != nullptr) {
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
  }

  if (method != "GET") {
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method.c_str());
  }
  if (!body.empty() || method == "PUT" || method == "POST") {
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }

  const CURLcode rc = curl_easy_perform(handle);

  long status = 0;
  curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);

  if (headers != nullptr) {
    curl_slist_free_all(headers);
  }
  curl_easy_cleanup(handle);

  if (rc != CURLE_OK) {
    return Response{};
  }
  resp.status = static_cast<int>(status);
  return resp;
}

Response Client::propfind(const std::string& path, int depth) {
  // 只请求这三项：openlist 的 PROPFIND 不接受 resourcetype 里带 collection 子元素，
  // 但接受自闭合的 <D:resourcetype/>，目录靠响应里的 <D:collection/> 判断。
  return request(
      "PROPFIND", path,
      {"Depth: " + std::to_string(depth), "Content-Type: application/xml; charset=utf-8"},
      "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:propfind xmlns:D=\"DAV:\"><D:prop>"
      "<D:resourcetype/><D:getcontentlength/><D:getlastmodified/></D:prop>"
      "</D:propfind>");
}

namespace {

// 一次 GET 传输的状态；析构时保证 curl 句柄被释放
struct StreamState {
  CURLM* multi = nullptr;
  CURL* easy = nullptr;
  curl_slist* slist = nullptr;
  std::string buffer;
  std::map<std::string, std::string> headers;
  int status = 0;
  bool headers_complete = false; // 当前响应头已收到空行
  bool done = false;
  bool failed = false;

  ~StreamState() {
    shutdown();
  }

  void shutdown() {
    if (multi != nullptr && easy != nullptr) {
      curl_multi_remove_handle(multi, easy);
    }
    if (easy != nullptr) {
      curl_easy_cleanup(easy);
      easy = nullptr;
    }
    if (slist != nullptr) {
      curl_slist_free_all(slist);
      slist = nullptr;
    }
    if (multi != nullptr) {
      curl_multi_cleanup(multi);
      multi = nullptr;
    }
    buffer.clear();
    done = true;
  }
};

std::size_t streamWrite(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* state = static_cast<StreamState*>(userdata);
  state->buffer.append(ptr, size * nmemb);
  return size * nmemb;
}

std::size_t streamHeader(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* state = static_cast<StreamState*>(userdata);
  const std::size_t len = size * nmemb;
  std::string line(ptr, len);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  if (line.rfind("HTTP/", 0) == 0) {
    // "HTTP/1.1 206 Partial Content"；重定向/认证重试会出现多行状态行
    state->headers.clear();
    // 中间响应的 body（若有）属于重定向前那一跳，不能混进最终文件字节
    state->buffer.clear();
    state->headers_complete = false;
    const std::size_t first = line.find(' ');
    if (first != std::string::npos) {
      state->status = static_cast<int>(std::strtol(line.c_str() + first + 1, nullptr, 10));
    }
    return len;
  }
  if (line.empty()) {
    state->headers_complete = true;
    return len;
  }
  absorbHeaderLine(state->headers, ptr, len);
  return len;
}

} // namespace

Stream openStream(const Url& base, const std::string& path, const std::vector<std::string>& extra,
                  const ClientOptions& opts) {
  ensureCurlGlobal();

  auto state = std::make_shared<StreamState>();
  state->multi = curl_multi_init();
  state->easy = curl_easy_init();
  if (state->multi == nullptr || state->easy == nullptr) {
    return Stream{};
  }

  applyCommonOptions(state->easy, base, opts, buildUrl(base, encodeSpaces(path)));
  curl_easy_setopt(state->easy, CURLOPT_WRITEFUNCTION, streamWrite);
  curl_easy_setopt(state->easy, CURLOPT_WRITEDATA, state.get());
  curl_easy_setopt(state->easy, CURLOPT_HEADERFUNCTION, streamHeader);
  curl_easy_setopt(state->easy, CURLOPT_HEADERDATA, state.get());

  curl_slist* headers = nullptr;
  for (const auto& h : extra) {
    headers = curl_slist_append(headers, h.c_str());
  }
  if (headers != nullptr) {
    curl_easy_setopt(state->easy, CURLOPT_HTTPHEADER, headers);
  }
  state->slist = headers;

  const bool added = curl_multi_add_handle(state->multi, state->easy) == CURLM_OK;
  if (!added) {
    return Stream{};
  }

  // 先推进到拿到「最终响应」为止。libcurl 会在同一个 transfer 内自己跟随 3xx，
  // 中间响应（1xx/3xx/401/407）的头不能当成结果返回，否则 Reader 会把 302 当失败。
  auto is_final = [](int status) {
    const int cls = status / 100;
    return status != 0 && cls != 1 && cls != 3 && status != 401 && status != 407;
  };
  while (!state->done && !(is_final(state->status) && state->headers_complete)) {
    int running = 0;
    if (curl_multi_perform(state->multi, &running) != CURLM_OK) {
      state->failed = true;
      break;
    }
    if (running == 0) {
      // transfer 真正结束：可能是最终响应，也可能是无法跟随的 3xx/认证失败
      state->done = true;
      break;
    }
    if (is_final(state->status) && state->headers_complete) {
      break;
    }
    int numfds = 0;
    if (curl_multi_poll(state->multi, nullptr, 0, 1000, &numfds) != CURLM_OK) {
      state->failed = true;
      break;
    }
  }

  if (state->failed || state->status == 0) {
    return Stream{};
  }

  Stream s;
  s.status = state->status;
  s.headers = state->headers;
  s.read = [state](char* buf, std::size_t n) -> long long {
    while (state->buffer.empty()) {
      if (state->done || state->failed) {
        return state->failed ? -1 : 0;
      }
      int running = 0;
      if (curl_multi_perform(state->multi, &running) != CURLM_OK) {
        state->failed = true;
        return -1;
      }
      if (running == 0) {
        state->done = true;
      }
      if (!state->buffer.empty()) {
        break;
      }
      if (state->done) {
        return 0;
      }
      int numfds = 0;
      if (curl_multi_poll(state->multi, nullptr, 0, 1000, &numfds) != CURLM_OK) {
        state->failed = true;
        return -1;
      }
    }
    const std::size_t take = std::min(n, state->buffer.size());
    std::memcpy(buf, state->buffer.data(), take);
    state->buffer.erase(0, take);
    return static_cast<long long>(take);
  };
  s.close = [state]() { state->shutdown(); };
  return s;
}

std::vector<Resource> parsePropfind(const std::string& xml) {
  std::vector<Resource> out;
  std::size_t pos = 0;
  while (true) {
    const std::size_t at = xml.find("response", pos);
    if (at == std::string::npos) {
      break;
    }
    const std::size_t open = xml.find('>', at);
    if (open == std::string::npos) {
      break;
    }
    std::size_t close = xml.find("</D:response>", open + 1);
    if (close == std::string::npos) {
      close = xml.find("</response>", open + 1);
    }
    if (close == std::string::npos) {
      close = xml.find("</", open + 1);
      if (close == std::string::npos) {
        break;
      }
    }
    const std::string block = xml.substr(open + 1, close - open - 1);
    pos = close;

    std::string href = xmlUnescape(trim(extractTag(block, "href")));
    if (href.empty()) {
      continue;
    }
    if (isWebdavUrl(href)) {
      const std::size_t sp = href.find("://");
      const std::size_t slash = href.find('/', sp + 3);
      href = (slash == std::string::npos) ? "/" : href.substr(slash);
    }
    if (href.size() > 1 && href.back() == '/') {
      href.pop_back();
    }

    Resource r;
    r.path = href; // 保持百分号编码，与请求路径一致
    r.collection = block.find("<D:collection") != std::string::npos ||
                   block.find("<collection") != std::string::npos;
    const std::string cl = trim(extractTag(block, "getcontentlength"));
    r.size = cl.empty() ? 0 : std::strtoll(cl.c_str(), nullptr, 10);
    r.mtime = httpDateToEpoch(trim(xmlUnescape(extractTag(block, "getlastmodified"))));
    out.push_back(std::move(r));
  }
  return out;
}

} // namespace sb::webdav