#pragma once

#include "config.hpp"
#include "result.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace soci {
class session;
class soci_error;
} // namespace soci

namespace sb {

// storage_nodes 的一行（Web 层收到的输入）
struct StorageNodeInput {
  std::string storage_id; // 空 = 新建，非空 = 更新（不存在则按该 ID 创建）
  std::string name;
  std::string type = "webdav";
  std::string endpoint;    // ip:port 或域名（不带 scheme/path）
  std::string config_json; // JSONB 文本（目前是 username/password/base_path）
  std::string state = "active";
};

struct StorageNodeResult {
  std::string storage_id;
  bool created = false;
};

// 从 storage_nodes 读回的完整一行（列表接口用）
struct StorageNodeRecord {
  std::string storage_id;
  std::string name;
  std::string type;
  std::string endpoint;
  std::string config_json;
  std::string state;
};

// 一个实体在某个存储节点上的一份位置（下载接口用）
struct EntityLocation {
  std::string entity_id;
  std::string sha256;
  long long size = 0;
  std::string mime_type;
  std::string storage_id;
  std::string key; // 相对 base_path 的路径

  // 所属存储节点（下载时不再回查 storage_nodes）
  std::string storage_name;
  std::string storage_type;
  std::string storage_endpoint;
  std::string storage_config_json;
  std::string storage_state;
};

// 哈希模式下，一个文件的「库内记录」判定结果
enum class LocationAction {
  Inserted, // entities 新建 + entity_locations 新建
  Attached, // entities 已存在，但该库还没有它的记录 -> 新增 location
  Updated,  // 同 key 之前指向别的实体（文件内容变了）-> 改指向
  Skipped,  // 该库已有这个内容的记录 -> 什么都不做
};

const char* to_string(LocationAction action);

struct FileRecordOutcome {
  LocationAction action = LocationAction::Skipped;
  std::string entity_id;
  bool entity_created = false;
};

// 远端 PostgreSQL 写入端（SOCI postgresql 后端）。
// 每个实例独占一条连接，只在持有它的那个线程里使用（由 DbPool 保证）。
class PgStore {
public:
  // 建立一条新连接；失败返回 Postgres 错误
  static Result<std::unique_ptr<PgStore>> connect(const DbConfig& cfg);

  explicit PgStore(std::unique_ptr<soci::session> db);
  ~PgStore();

  PgStore(const PgStore&) = delete;
  PgStore& operator=(const PgStore&) = delete;

  Status ping();

  Result<StorageNodeResult> upsert_storage(const StorageNodeInput& in);
  Result<StorageNodeRecord> get_storage(const std::string& storage_id);
  Result<std::vector<StorageNodeRecord>> list_storages();

  // 哈希模式写入口径（单文件、单事务）：
  //   1. entities 里按 sha256 找实体
  //   2. 找到：若该 storage 已有它的 location -> Skipped；否则补一行 location
  //   3. 找不到：先建 entities，再按 (storage_id, key) 决定 insert 还是改指向
  // key 是相对 base_path 的路径（与 file.list 的 rel_path 同款）。
  Result<FileRecordOutcome> record_file(const std::string& storage_id, const std::string& key,
                                        const std::string& sha256, long long size,
                                        const std::string& mime_type);

  // 校验模式用：按 (storage_id, 「上级目录/文件名」) 找已有位置，判断是否存在大小一致的实体。
  // 命中即认为「已扫描」，不需要再读内容算哈希。
  // suffix_key 是完整 key 的最后两级（如 "s1/f1.txt"）：既匹配根下的 f1.txt，
  // 也匹配任意深度的 .../s1/f1.txt。纯读，不写任何表。
  Result<bool> location_matches_size(const std::string& storage_id, const std::string& suffix_key,
                                     long long size);

  // 下载用：列出该实体的全部位置（含所属存储节点信息），按 storage 名 + key 排序。
  // 实体不存在或没有任何 location 时返回空数组（由调用方给 404）。
  Result<std::vector<EntityLocation>> list_entity_locations(const std::string& entity_id);

  // 批量判定「这些 key 在该存储上是否已有实体」：返回 key -> entity_id。
  // 内部按 500 个一批发 IN 查询；不关心 location 的 state / failed_at。
  Result<std::map<std::string, std::string>> lookup_locations(
      const std::string& storage_id, const std::vector<std::string>& keys);

  // 下载探测的记账：只动 failed_at，不碰 state / verified_at
  Status mark_location_ok(const std::string& entity_id, const std::string& storage_id,
                          const std::string& key);
  Status mark_location_failed(const std::string& entity_id, const std::string& storage_id,
                              const std::string& key);

private:
  PgStore() = default;

  // SOCI 异常 → Error；postgresql_soci_error 的 SQLSTATE 放进 detail
  static Error translate(const std::string& what, const soci::soci_error& e,
                         const std::string& context);

  std::unique_ptr<soci::session> db_;
};

} // namespace sb