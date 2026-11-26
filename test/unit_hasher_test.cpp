#include "hasher.hpp"
#include "support/assertions.hpp"

#include <gtest/gtest.h>

#include <string>

namespace sb {
namespace {

TEST(UnitHasher, Empty) {
  Sha256 hasher;
  auto digest = hasher.hex_final();
  ASSERT_TRUE(digest.ok());
  EXPECT_EQ(*digest, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(UnitHasher, Abc) {
  Sha256 hasher;
  ASSERT_TRUE(hasher.update("abc", 3).ok());
  auto digest = hasher.hex_final();
  ASSERT_TRUE(digest.ok());
  EXPECT_EQ(*digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(UnitHasher, SplitUpdatesMatchSingleUpdate) {
  Sha256 single;
  ASSERT_TRUE(single.update("hello world", 11).ok());
  auto expected = single.hex_final();

  Sha256 split;
  ASSERT_TRUE(split.update("hello", 5).ok());
  ASSERT_TRUE(split.update(" ", 1).ok());
  ASSERT_TRUE(split.update("world", 5).ok());
  auto actual = split.hex_final();

  ASSERT_TRUE(expected.ok());
  ASSERT_TRUE(actual.ok());
  EXPECT_EQ(*actual, *expected);
}

TEST(UnitHasher, HexFinalTwiceFails) {
  Sha256 hasher;
  ASSERT_TRUE(hasher.hex_final().ok());
  expect_error(hasher.hex_final(), ErrorCode::Internal);
}

} // namespace
} // namespace sb