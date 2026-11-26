#include "support/assertions.hpp"
#include "util.hpp"

#include <gtest/gtest.h>

#include <string>

namespace sb {
namespace {

struct UriCase {
  const char* input;
  const char* expected;
};

TEST(UnitUtil, NormalizeUri) {
  const UriCase cases[] = {
      {"http://h/a//b/?x=1", "http://h/a/b"},
      {"http://h/", "http://h"},
      {"http://h//a///b/", "http://h/a/b"},
      {"/a//b/", "/a/b"},
      {"", ""},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.input);
    EXPECT_EQ(normalize_uri(c.input), c.expected);
  }
}

TEST(UnitUtil, JoinUri) {
  EXPECT_EQ(join_uri("http://h/base", "f.txt"), "http://h/base/f.txt");
  EXPECT_EQ(join_uri("http://h/base/", "/f.txt"), "http://h/base/f.txt");
  EXPECT_EQ(join_uri("http://h/base//", "sub//f.txt"), "http://h/base/sub/f.txt");
  EXPECT_EQ(join_uri("http://h/base", ""), "http://h/base");
}

TEST(UnitUtil, RedactUri) {
  EXPECT_EQ(redact_uri("http://h/p?a=1&b=2"), "http://h/p?...a=***&b=***");
  EXPECT_EQ(redact_uri("http://h/p?flag"), "http://h/p?...flag=***");
  EXPECT_EQ(redact_uri("http://h/p"), "http://h/p");
}

TEST(UnitUtil, BasenameOf) {
  EXPECT_EQ(basename_of("http://h/a/b.flac"), "b.flac");
  EXPECT_EQ(basename_of("http://h/a/b.flac?x=1"), "b.flac");
  EXPECT_EQ(basename_of("http://h/a/b/"), "b");
  EXPECT_EQ(basename_of("plain.txt"), "plain.txt");
}

TEST(UnitUtil, MimeTypeFor) {
  EXPECT_EQ(mime_type_for("a.flac"), "audio/flac");
  EXPECT_EQ(mime_type_for("A.FLAC"), "audio/flac");
  EXPECT_EQ(mime_type_for("/dav/a.jpg"), "image/jpeg");
  EXPECT_EQ(mime_type_for("a.unknown"), "unknown");
  EXPECT_EQ(mime_type_for("noext"), "application/octet-stream");
}

TEST(UnitUtil, TrimAndLower) {
  EXPECT_EQ(trim("  abc \t\n"), "abc");
  EXPECT_EQ(trim("   "), "");
  EXPECT_EQ(to_lower("AbC-123"), "abc-123");
}

} // namespace
} // namespace sb