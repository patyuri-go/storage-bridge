#include "scan_job.hpp"
#include "storage.hpp"
#include "support/assertions.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace sb {
namespace {

TEST(UnitScanJob, TwoLevelKey) {
  EXPECT_EQ(two_level_key(""), "");
  EXPECT_EQ(two_level_key("/"), "");
  EXPECT_EQ(two_level_key("/f.txt"), "f.txt");
  EXPECT_EQ(two_level_key("/s1/f.txt"), "s1/f.txt");
  EXPECT_EQ(two_level_key("/a/b/c.flac"), "b/c.flac");
  EXPECT_EQ(two_level_key("a//b///c"), "b/c");
}

TEST(UnitScanJob, JoinKey) {
  EXPECT_EQ(join_key("/a", "b"), "/a/b");
  EXPECT_EQ(join_key("/", "b"), "/b");
  EXPECT_EQ(join_key("", "b"), "/b");
  EXPECT_EQ(join_key("/a", ""), "/a");
}

TEST(UnitScanJob, RejectsInvalidContextOrUri) {
  auto missing_ctx = make_scan_file_job(nullptr, "http://h/a", "/a");
  expect_error(missing_ctx, ErrorCode::InvalidArgument, "storage");

  auto ctx = std::make_shared<ScanContext>();
  auto created = Storage::create(StorageOptions{});
  ASSERT_TRUE(created.ok());
  ctx->storage = std::shared_ptr<Storage>(std::move(*created));
  expect_error(make_scan_file_job(ctx, "", "/a"), ErrorCode::InvalidArgument, "file_uri");
  expect_error(make_scan_file_job(ctx, "no-scheme/a", "/a"), ErrorCode::InvalidArgument,
               "完整 URL");
  expect_error(make_scan_folder_job(ctx, "http://h/a", ""), ErrorCode::InvalidArgument,
               "folder_key");
}

TEST(UnitScanJob, BuildsJobs) {
  auto created = Storage::create(StorageOptions{});
  ASSERT_TRUE(created.ok());
  std::shared_ptr<Storage> storage(std::move(*created));
  auto ctx = std::make_shared<ScanContext>();
  ctx->storage = storage;
  ctx->storage_id = "sid";

  auto file = make_scan_file_job(ctx, "http://h/a.flac", "/a.flac");
  ASSERT_TRUE(file.ok());
  EXPECT_NE(*file, nullptr);

  auto folder = make_scan_folder_job(ctx, "http://h/dir/", "/dir");
  ASSERT_TRUE(folder.ok());
  EXPECT_NE(*folder, nullptr);
}

} // namespace
} // namespace sb