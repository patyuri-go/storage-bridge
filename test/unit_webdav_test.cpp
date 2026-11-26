#include "support/assertions.hpp"
#include "webdav.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace sb::webdav {
namespace {

TEST(UnitWebdav, IsWebdavUrl) {
  EXPECT_TRUE(isWebdavUrl("http://example.com"));
  EXPECT_TRUE(isWebdavUrl("HTTPS://example.com/a"));
  EXPECT_FALSE(isWebdavUrl("ftp://example.com"));
  EXPECT_FALSE(isWebdavUrl("example.com"));
}

TEST(UnitWebdav, ParseUrl) {
  Url url;
  ASSERT_TRUE(parseUrl("http://example.com/a/b", url));
  EXPECT_FALSE(url.https);
  EXPECT_EQ(url.host, "example.com");
  EXPECT_EQ(url.port, "");
  EXPECT_EQ(url.path, "/a/b");

  Url secure;
  ASSERT_TRUE(parseUrl("https://user:p%40ss@example.com:8443/x", secure));
  EXPECT_TRUE(secure.https);
  EXPECT_EQ(secure.host, "example.com");
  EXPECT_EQ(secure.port, "8443");
  EXPECT_EQ(secure.user, "user");
  EXPECT_EQ(secure.pass, "p@ss");
  EXPECT_EQ(secure.path, "/x");

  Url ipv6;
  ASSERT_TRUE(parseUrl("http://[::1]:8080/dav", ipv6));
  EXPECT_EQ(ipv6.host, "[::1]");
  EXPECT_EQ(ipv6.port, "8080");
  EXPECT_EQ(ipv6.path, "/dav");

  Url root;
  ASSERT_TRUE(parseUrl("http://h", root));
  EXPECT_EQ(root.path, "/");
}

TEST(UnitWebdav, ParseUrlRejectsInvalid) {
  Url url;
  EXPECT_FALSE(parseUrl("ftp://example.com/x", url));
  EXPECT_FALSE(parseUrl("http:///x", url));
  EXPECT_FALSE(parseUrl("example.com/x", url));
  EXPECT_FALSE(parseUrl("http://[::1/x", url));
}

TEST(UnitWebdav, PercentDecode) {
  EXPECT_EQ(percentDecode("%E4%B8%AD%20%5Bx%5D"), "中 [x]");
  EXPECT_EQ(percentDecode("a%2Fb"), "a/b");
  EXPECT_EQ(percentDecode("%ZZ"), "%ZZ");
  EXPECT_EQ(percentDecode("%2"), "%2");
}

TEST(UnitWebdav, BuildUrl) {
  Url base;
  base.host = "h";
  base.port = "8080";
  EXPECT_EQ(buildUrl(base, "/a%20b"), "http://h:8080/a%20b");
  EXPECT_EQ(buildUrl(base, ""), "http://h:8080/");

  base.https = true;
  EXPECT_EQ(buildUrl(base, "/x"), "https://h:8080/x");
}

TEST(UnitWebdav, HttpDateToEpoch) {
  EXPECT_EQ(httpDateToEpoch("Wed, 24 Jun 2026 03:17:54 GMT"), 1782271074);
  EXPECT_EQ(httpDateToEpoch(""), 0);
  EXPECT_EQ(httpDateToEpoch("not a date"), 0);
  EXPECT_EQ(httpDateToEpoch("Wed, 24 Xxx 2026 03:17:54 GMT"), 0);
}

TEST(UnitWebdav, ParsePropfindFile) {
  const std::string xml = R"(<?xml version="1.0"?>
<D:multistatus xmlns:D="DAV:">
  <D:response>
    <D:href>http://example.com/dav/a%20b.flac</D:href>
    <D:propstat>
      <D:prop>
        <D:resourcetype/>
        <D:getcontentlength>1234</D:getcontentlength>
        <D:getlastmodified>Wed, 24 Jun 2026 03:17:54 GMT</D:getlastmodified>
      </D:prop>
    </D:propstat>
  </D:response>
</D:multistatus>)";
  const auto items = parsePropfind(xml);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].path, "/dav/a%20b.flac");
  EXPECT_FALSE(items[0].collection);
  EXPECT_EQ(items[0].size, 1234);
  EXPECT_EQ(items[0].mtime, 1782271074);
}

TEST(UnitWebdav, ParsePropfindCollection) {
  const std::string xml = R"(<?xml version="1.0"?>
<D:multistatus xmlns:D="DAV:">
  <D:response>
    <D:href>/dav/</D:href>
    <D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop></D:propstat>
  </D:response>
</D:multistatus>)";
  const auto items = parsePropfind(xml);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].path, "/dav");
  EXPECT_TRUE(items[0].collection);
  EXPECT_EQ(items[0].size, 0);
}

TEST(UnitWebdav, ParsePropfindIgnoresResponseWithoutHref) {
  const std::string xml = R"(<multistatus><response><propstat/></response></multistatus>)";
  EXPECT_TRUE(parsePropfind(xml).empty());
}

} // namespace
} // namespace sb::webdav