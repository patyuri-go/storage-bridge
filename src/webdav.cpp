#define ASIO_STANDALONE

#include "webdav.hpp"

#include "util.hpp"

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <asio/ssl/error.hpp>
#include <openssl/ssl.h>

#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <utility>

namespace sb::webdav {

namespace {

constexpr const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const std::string& in) {
  std::string out;
  int val = 0;
  int bits = -6;
  for (const unsigned char c : in) {
    val = (val << 8) + c;
    bits += 8;
    while (bits >= 0) {
      out.push_back(kB64[(val >> bits) & 0x3F]);
      bits -= 6;
    }
  }
  if (bits > -6) {
    out.push_back(kB64[((val << 8) >> (bits + 8)) & 0x3F]);
  }
  while (out.size() % 4 != 0) {
    out.push_back('=');
  }
  return out;
}

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

void setTimeout(asio::ip::tcp::socket& sock, int seconds) {
  struct timeval tv {};
  tv.tv_sec = seconds;
  setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

template <typename SyncStream> std::string readAll(SyncStream& stream) {
  std::string out;
  char buf[16384];
  std::error_code ec;
  for (;;) {
    const std::size_t n = stream.read_some(asio::buffer(buf, sizeof(buf)), ec);
    if (n > 0) {
      out.append(buf, n);
    }
    if (ec) {
      break;
    }
  }
  return out;
}

std::string dechunk(const std::string& in) {
  std::string out;
  std::size_t pos = 0;
  while (pos < in.size()) {
    const std::size_t eol = in.find("\r\n", pos);
    if (eol == std::string::npos) {
      break;
    }
    const long n = std::strtol(in.substr(pos, eol - pos).c_str(), nullptr, 16);
    if (n <= 0) {
      break;
    }
    pos = eol + 2;
    out.append(in, pos, static_cast<std::size_t>(n));
    pos += static_cast<std::size_t>(n) + 2;
  }
  return out;
}

Response parseResponse(const std::string& raw) {
  Response res;
  const std::size_t head_end = raw.find("\r\n\r\n");
  if (head_end == std::string::npos) {
    return res;
  }
  std::istringstream head(raw.substr(0, head_end));
  std::string line;
  std::getline(head, line);
  {
    const std::size_t sp = line.find(' ');
    if (sp != std::string::npos) {
      res.status = std::atoi(line.substr(sp + 1).c_str());
    }
  }
  while (std::getline(head, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos) {
      res.headers[to_lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
  }
  std::string body = raw.substr(head_end + 4);
  const auto te = res.headers.find("transfer-encoding");
  if (te != res.headers.end() && to_lower(te->second).find("chunked") != std::string::npos) {
    body = dechunk(body);
  }
  res.body = std::move(body);
  return res;
}

template <typename SyncStream> Response doExchange(SyncStream& stream, const std::string& request) {
  std::error_code ec;
  asio::write(stream, asio::buffer(request), ec);
  if (ec) {
    return Response{};
  }
  return parseResponse(readAll(stream));
}

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

std::string buildRequestString(const Url& target, const ClientOptions& opts,
                               const std::string& method, const std::string& path,
                               const std::vector<std::string>& extra, const std::string& body,
                               bool send_auth) {
  std::string safe_path = path;
  for (std::size_t i = 0; i < safe_path.size(); ++i) {
    if (safe_path[i] == ' ') {
      safe_path.replace(i, 1, "%20");
      i += 2;
    }
  }

  std::string req = method + " " + safe_path + " HTTP/1.1\r\n";
  req += "Host: " + target.host + "\r\n";
  if (send_auth && (!target.user.empty() || !target.pass.empty())) {
    req += "Authorization: Basic " + base64(target.user + ":" + target.pass) + "\r\n";
  }
  req += "User-Agent: " + opts.user_agent + "\r\n";
  req += "Accept: */*\r\n";
  for (const auto& h : extra) {
    req += h + "\r\n";
  }
  if (!body.empty() || method == "PUT" || method == "POST") {
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  req += "Connection: close\r\n\r\n";
  req += body;
  return req;
}

void setupTls(asio::ssl::context& ctx, bool verify) {
  if (verify) {
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(asio::ssl::verify_peer);
  } else {
    ctx.set_verify_mode(asio::ssl::verify_none);
  }
}

// 解析绝对 Location（http://host:port/path）到 Url
bool parseAbsoluteLocation(const std::string& loc, Url& out) {
  if (!parseUrl(loc, out)) {
    return false;
  }
  out.user.clear();
  out.pass.clear();
  return true;
}

Response doRequest(const Url& target, const ClientOptions& opts, const std::string& method,
                   const std::string& path, const std::vector<std::string>& extra,
                   const std::string& body, bool send_auth) {
  asio::io_context ioc;
  asio::ip::tcp::resolver resolver(ioc);
  const std::string port = target.port.empty() ? (target.https ? "443" : "80") : target.port;

  std::error_code ec;
  const auto results = resolver.resolve(target.host, port, ec);
  if (ec) {
    return Response{};
  }

  const std::string req = buildRequestString(target, opts, method, path, extra, body, send_auth);

  if (target.https) {
    asio::ssl::context ctx(asio::ssl::context::tls_client);
    setupTls(ctx, opts.ssl_verify);
    asio::ssl::stream<asio::ip::tcp::socket> stream(ioc, ctx);
    asio::connect(stream.next_layer(), results, ec);
    if (ec) {
      return Response{};
    }
    setTimeout(stream.next_layer(), opts.timeout);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), target.host.c_str())) {
      return Response{};
    }
    stream.handshake(asio::ssl::stream_base::client, ec);
    if (ec) {
      return Response{};
    }
    return doExchange(stream, req);
  }

  asio::ip::tcp::socket sock(ioc);
  asio::connect(sock, results, ec);
  if (ec) {
    return Response{};
  }
  setTimeout(sock, opts.timeout);
  return doExchange(sock, req);
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
  Response resp = doRequest(base_, opts_, method, path, extra, body, true);
  int redirects = 0;
  while (isRedirect(resp.status) && redirects < 5) {
    const std::string loc = resp.header("location");
    if (loc.empty()) {
      break;
    }
    Url next = base_;
    std::string next_path = loc;
    if (isWebdavUrl(loc)) {
      if (!parseAbsoluteLocation(loc, next)) {
        break;
      }
      next_path = next.path;
    } else if (!loc.empty() && loc.front() != '/') {
      const std::size_t slash = base_.path.rfind('/');
      next_path =
          (slash == std::string::npos ? std::string("/") : base_.path.substr(0, slash + 1)) + loc;
    }
    const bool same =
        (next.host == base_.host && next.port == base_.port && next.https == base_.https);
    resp = doRequest(next, opts_, method, next_path, extra, body, same);
    ++redirects;
  }
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

void parseStreamHead(const std::string& head, Stream& s) {
  const std::size_t line_end = head.find("\r\n");
  const std::string status_line =
      head.substr(0, line_end == std::string::npos ? head.size() : line_end);
  const std::size_t sp = status_line.find(' ');
  if (sp != std::string::npos) {
    s.status = std::atoi(status_line.c_str() + sp + 1);
  }
  std::size_t pos = (line_end == std::string::npos) ? head.size() : line_end + 2;
  while (pos < head.size()) {
    const std::size_t e = head.find("\r\n", pos);
    if (e == std::string::npos) {
      break;
    }
    const std::string line = head.substr(pos, e - pos);
    if (line.empty()) {
      break;
    }
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos) {
      s.headers[to_lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    pos = e + 2;
  }
}

template <typename StreamT>
Stream openOnStream(std::shared_ptr<StreamT> stream, const std::string& req) {
  std::error_code ec;
  asio::write(*stream, asio::buffer(req), ec);
  if (ec) {
    return Stream{};
  }
  auto pending = std::make_shared<std::string>();
  char tmp[16384];
  while (true) {
    const std::size_t got = stream->read_some(asio::buffer(tmp, sizeof(tmp)), ec);
    if (ec || got == 0) {
      return Stream{};
    }
    pending->append(tmp, got);
    if (pending->find("\r\n\r\n") != std::string::npos) {
      break;
    }
    if (pending->size() > 131072) {
      return Stream{};
    }
  }
  const std::size_t head_end = pending->find("\r\n\r\n");
  const std::string head = pending->substr(0, head_end + 2);
  *pending = pending->substr(head_end + 4);

  Stream s;
  parseStreamHead(head, s);
  s.read = [stream, pending](char* buf, std::size_t n) -> long long {
    if (!pending->empty()) {
      const std::size_t take = std::min(n, pending->size());
      std::memcpy(buf, pending->data(), take);
      pending->erase(0, take);
      return static_cast<long long>(take);
    }
    std::error_code read_ec;
    const std::size_t got = stream->read_some(asio::buffer(buf, n), read_ec);
    // 对端把连接关掉是正常 EOF，不是错误（服务器普遍带 Connection: close）
    if (read_ec == asio::error::eof || read_ec == asio::ssl::error::stream_truncated) {
      return 0;
    }
    if (read_ec) {
      return -1;
    }
    return static_cast<long long>(got);
  };
  s.close = [stream]() {
    std::error_code close_ec;
    stream->lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, close_ec);
    stream->lowest_layer().close(close_ec);
  };
  return s;
}

} // namespace

Stream openStream(const Url& base, const std::string& path, const std::vector<std::string>& extra,
                  const ClientOptions& opts) {
  Url target = base;
  std::string req_path = path;
  bool send_auth = true;

  for (int attempt = 0; attempt < 6; ++attempt) {
    const std::string port = target.port.empty() ? (target.https ? "443" : "80") : target.port;
    // ioc 必须活到 socket 析构之后，否则 socket 析构时会去访问已释放的 reactor。
    // 这里让它挂在 socket 的自定义 deleter 上，比 ioc 是局部变量活得久。
    auto ioc = std::make_shared<asio::io_context>();
    asio::ip::tcp::resolver resolver(*ioc);
    std::error_code ec;
    const auto results = resolver.resolve(target.host, port, ec);
    if (ec) {
      return Stream{};
    }
    const std::string req = buildRequestString(target, opts, "GET", req_path, extra, "", send_auth);

    Stream s;
    if (target.https) {
      auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
      setupTls(*ctx, opts.ssl_verify);
      using TlsSocket = asio::ssl::stream<asio::ip::tcp::socket>;
      std::shared_ptr<TlsSocket> stream(new TlsSocket(*ioc, *ctx),
                                        [ioc, ctx](TlsSocket* p) { delete p; });
      asio::connect(stream->next_layer(), results, ec);
      if (ec) {
        return Stream{};
      }
      setTimeout(stream->next_layer(), opts.timeout);
      if (!SSL_set_tlsext_host_name(stream->native_handle(), target.host.c_str())) {
        return Stream{};
      }
      stream->handshake(asio::ssl::stream_base::client, ec);
      if (ec) {
        return Stream{};
      }
      s = openOnStream(stream, req);
    } else {
      using PlainSocket = asio::ip::tcp::socket;
      std::shared_ptr<PlainSocket> stream(new PlainSocket(*ioc),
                                          [ioc](PlainSocket* p) { delete p; });
      asio::connect(*stream, results, ec);
      if (ec) {
        return Stream{};
      }
      setTimeout(*stream, opts.timeout);
      s = openOnStream(stream, req);
    }

    if (s.status == 0) {
      return Stream{};
    }
    if (!isRedirect(s.status)) {
      return s;
    }

    const auto it = s.headers.find("location");
    const std::string location = (it == s.headers.end()) ? std::string() : it->second;
    s.reset();
    if (location.empty()) {
      return Stream{};
    }

    Url next;
    if (isWebdavUrl(location)) {
      if (!parseAbsoluteLocation(location, next)) {
        return Stream{};
      }
    } else {
      next = target;
      if (location.front() == '/') {
        next.path = location;
      } else {
        const std::size_t slash = target.path.rfind('/');
        next.path =
            (slash == std::string::npos ? std::string("/") : target.path.substr(0, slash + 1)) +
            location;
      }
    }
    req_path = next.path;
    target = next;
    send_auth = (next.host == base.host && next.port == base.port && next.https == base.https);
  }
  return Stream{};
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
