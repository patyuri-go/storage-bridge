#include "pg_store.hpp"

#include "util.hpp"

#include <json/json.h>

#include <soci/postgresql/soci-postgresql.h>
#include <soci/soci.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <string>

namespace sb {

const char* to_string(LocationAction action) {
  switch (action) {
  case LocationAction::Inserted:
    return "inserted";
  case LocationAction::Attached:
    return "attached";
  case LocationAction::Updated:
    return "updated";
  case LocationAction::Skipped:
    return "skipped";
  }
  return "unknown";
}

Error PgStore::translate(const std::string& what, const soci::soci_error& e,
                         const std::string& context) {
  std::string code;
  if (const auto* pg = dynamic_cast<const soci::postgresql_soci_error*>(&e)) {
    code = pg->sqlstate();
  }
  std::string msg = what;
  if (const char* w = e.what(); w != nullptr && *w != '\0') {
    msg += ": ";
    msg += w;
  }
  return err(ErrorCode::Postgres, std::move(msg), std::move(code), context);
}

Result<std::unique_ptr<PgStore>> PgStore::connect(const DbConfig& cfg) {
  try {
    auto db = std::make_unique<soci::session>(soci::postgresql, cfg.conninfo());
    std::unique_ptr<PgStore> self(new PgStore(std::move(db)));
    LOG_INFO << cat("postgres connected: ", cfg.redacted());
    return self;
  } catch (const soci::soci_error& e) {
    return translate("连接 PostgreSQL 失败", e, cfg.redacted());
  }
}

PgStore::PgStore(std::unique_ptr<soci::session> db) : db_(std::move(db)) {}

PgStore::~PgStore() = default;

Status PgStore::ping() {
  try {
    int one = 0;
    *db_ << "select 1", soci::into(one);
  } catch (const soci::soci_error& e) {
    return translate("ping 失败", e, "");
  }
  return {};
}

Result<StorageNodeResult> PgStore::upsert_storage(const StorageNodeInput& in) {
  auto abort = [this](Error e) -> Result<StorageNodeResult> {
    try {
      db_->rollback();
    } catch (const soci::soci_error& re) {
      LOG_WARN << cat("postgres rollback 失败: ", re.what());
    }
    return e;
  };

  try {
    db_->begin();

    bool exists = false;
    if (!in.storage_id.empty()) {
      int n = 0;
      *db_ << "select count(*) from storage_nodes where storage_id = CAST(:id AS uuid)",
          soci::into(n), soci::use(in.storage_id);
      exists = n > 0;
    }

    StorageNodeResult out;
    out.storage_id = in.storage_id;

    if (exists) {
      soci::statement st =
          (db_->prepare << "update storage_nodes set name = :name, type = :type, "
                           "endpoint = :endpoint, config = CAST(:cfg AS jsonb), state = :state "
                           "where storage_id = CAST(:id AS uuid)",
           soci::use(in.name), soci::use(in.type), soci::use(in.endpoint),
           soci::use(in.config_json), soci::use(in.state), soci::use(in.storage_id));
      st.execute(true);
      if (st.get_affected_rows() != 1) {
        return abort(err(ErrorCode::Postgres, "更新后未命中 storage_id", "", in.storage_id));
      }
      out.created = false;
    } else {
      if (out.storage_id.empty()) {
        *db_ << "select gen_random_uuid()::text", soci::into(out.storage_id);
      }
      *db_ << "insert into storage_nodes(storage_id, name, type, endpoint, config, state) "
              "values(CAST(:id AS uuid), :name, :type, :endpoint, CAST(:cfg AS jsonb), :state)",
          soci::use(out.storage_id), soci::use(in.name), soci::use(in.type), soci::use(in.endpoint),
          soci::use(in.config_json), soci::use(in.state);
      out.created = true;
    }

    db_->commit();
    return out;
  } catch (const soci::soci_error& e) {
    return abort(translate("写入 storage_nodes 失败", e, in.name));
  }
}

Result<StorageNodeRecord> PgStore::get_storage(const std::string& storage_id) {
  try {
    StorageNodeRecord rec;
    std::string cfg;
    soci::indicator cfg_ind = soci::i_null;
    soci::statement st =
        (db_->prepare << "select name, type, endpoint, CAST(config AS text), state "
                         "from storage_nodes where storage_id = CAST(:id AS uuid)",
         soci::into(rec.name), soci::into(rec.type), soci::into(rec.endpoint),
         soci::into(cfg, cfg_ind), soci::into(rec.state), soci::use(storage_id));
    st.execute(false);
    if (!st.fetch()) {
      return err(ErrorCode::NotFound, "storage 不存在", "", storage_id);
    }
    rec.storage_id = storage_id;
    rec.config_json = (cfg_ind == soci::i_ok) ? cfg : std::string("{}");
    return rec;
  } catch (const soci::soci_error& e) {
    return translate("查询 storage_nodes 失败", e, storage_id);
  }
}

Result<std::vector<StorageNodeRecord>> PgStore::list_storages() {
  try {
    StorageNodeRecord rec;
    std::string cfg;
    soci::indicator cfg_ind = soci::i_null;
    soci::statement st =
        (db_->prepare
             << "select storage_id::text, name, type, endpoint, CAST(config AS text), state "
                "from storage_nodes order by name, storage_id",
         soci::into(rec.storage_id), soci::into(rec.name), soci::into(rec.type),
         soci::into(rec.endpoint), soci::into(cfg, cfg_ind), soci::into(rec.state));
    st.execute(false);

    std::vector<StorageNodeRecord> out;
    while (st.fetch()) {
      StorageNodeRecord item = rec;
      item.config_json = (cfg_ind == soci::i_ok) ? cfg : std::string("{}");
      out.push_back(std::move(item));
    }
    return out;
  } catch (const soci::soci_error& e) {
    return translate("查询 storage_nodes 列表失败", e, "");
  }
}

Result<FileRecordOutcome> PgStore::record_file(const std::string& storage_id,
                                               const std::string& key, const std::string& sha256,
                                               long long size, const std::string& mime_type) {
  auto rollback_quietly = [this] {
    try {
      db_->rollback();
    } catch (const soci::soci_error& re) {
      LOG_WARN << cat("postgres rollback 失败: ", re.what());
    }
  };

  // 多个 worker 可能同时算出同一个新哈希，撞 entities_sha256_key 后重试
  constexpr int kMaxAttempts = 4;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    try {
      db_->begin();

      FileRecordOutcome out;

      long long known = 0;
      *db_ << "select count(*) from entities where sha256 = :h", soci::into(known),
          soci::use(sha256);

      if (known > 0) {
        *db_ << "select entity_id::text from entities where sha256 = :h", soci::into(out.entity_id),
            soci::use(sha256);

        // 该库是否已经记录过这个实体（不论 key）
        long long on_storage = 0;
        *db_ << "select count(*) from entity_locations where entity_id = CAST(:e AS uuid) "
                "and storage_id = CAST(:s AS uuid)",
            soci::into(on_storage), soci::use(out.entity_id), soci::use(storage_id);
        if (on_storage > 0) {
          out.action = LocationAction::Skipped;
          db_->commit();
          return out;
        }
      } else {
        *db_ << "insert into entities(sha256, size, mime_type) values(:h, :size, :mime)",
            soci::use(sha256), soci::use(size), soci::use(mime_type);
        *db_ << "select entity_id::text from entities where sha256 = :h", soci::into(out.entity_id),
            soci::use(sha256);
        out.entity_created = true;
      }

      // 同一个 key 上原本指向哪个实体（文件内容变了就会指向旧实体）
      std::string old_entity;
      soci::indicator old_ind = soci::i_null;
      soci::statement look =
          (db_->prepare << "select entity_id::text from entity_locations "
                           "where storage_id = CAST(:s AS uuid) and key = :k",
           soci::into(old_entity, old_ind), soci::use(storage_id), soci::use(key));
      look.execute(false);
      if (!look.fetch()) {
        old_ind = soci::i_null;
        old_entity.clear();
      }

      if (old_ind == soci::i_ok) {
        soci::statement up =
            (db_->prepare << "update entity_locations set entity_id = CAST(:e AS uuid), "
                             "state = 'available', verified_at = now(), failed_at = null "
                             "where storage_id = CAST(:s AS uuid) and key = :k",
             soci::use(out.entity_id), soci::use(storage_id), soci::use(key));
        up.execute(true);
        if (up.get_affected_rows() != 1) {
          rollback_quietly();
          return err(ErrorCode::Postgres, "更新 entity_locations 未命中 (storage_id, key)", "",
                     key);
        }
        out.action =
            (old_entity == out.entity_id) ? LocationAction::Skipped : LocationAction::Updated;
      } else {
        *db_ << "insert into entity_locations(entity_id, storage_id, key, state, verified_at) "
                "values(CAST(:e AS uuid), CAST(:s AS uuid), :k, 'available', now())",
            soci::use(out.entity_id), soci::use(storage_id), soci::use(key);
        out.action = out.entity_created ? LocationAction::Inserted : LocationAction::Attached;
      }

      db_->commit();
      return out;
    } catch (const soci::soci_error& e) {
      Error translated = translate("写入 entities / entity_locations 失败", e, key);
      const bool retryable = (translated.detail == "23505") && attempt + 1 < kMaxAttempts;
      rollback_quietly();
      if (retryable) {
        continue;
      }
      return translated;
    }
  }

  return err(ErrorCode::Postgres,
             "写入 entities / entity_locations 失败（唯一约束冲突重试后仍失败）", "23505", key);
}

Result<bool> PgStore::location_matches_size(const std::string& storage_id,
                                            const std::string& suffix_key, long long size) {
  try {
    // 根下的文件 key 就是 "/f1.txt"，深层的是 "/a/b/f1.txt"；两种都算命中
    const std::string pattern = cat("%/", suffix_key);
    long long hits = 0;
    *db_ << "select count(*) from entities e "
            "join entity_locations l on l.entity_id = e.entity_id "
            "where l.storage_id = CAST(:s AS uuid) "
            "and (l.key = :k or l.key like :pat) "
            "and e.size = :size",
        soci::into(hits), soci::use(storage_id), soci::use(suffix_key), soci::use(pattern),
        soci::use(size);
    return hits > 0;
  } catch (const soci::soci_error& e) {
    return translate("查询 entity_locations 失败", e, suffix_key);
  }
}

Result<std::vector<EntityLocation>> PgStore::list_entity_locations(const std::string& entity_id) {
  try {
    std::vector<EntityLocation> out;
    EntityLocation loc;
    soci::indicator mime_ind = soci::i_null;
    soci::indicator cfg_ind = soci::i_null;
    soci::statement st =
        (db_->prepare << "select e.entity_id::text, e.sha256, e.size, e.mime_type, "
                         "l.storage_id::text, l.key, "
                         "s.name, s.type, s.endpoint, CAST(s.config AS text), s.state "
                         "from entities e "
                         "join entity_locations l on l.entity_id = e.entity_id "
                         "join storage_nodes s on s.storage_id = l.storage_id "
                         "where e.entity_id = CAST(:id AS uuid) "
                         "order by s.name, l.key",
         soci::into(loc.entity_id), soci::into(loc.sha256), soci::into(loc.size),
         soci::into(loc.mime_type, mime_ind), soci::into(loc.storage_id), soci::into(loc.key),
         soci::into(loc.storage_name), soci::into(loc.storage_type), soci::into(loc.storage_endpoint),
         soci::into(loc.storage_config_json, cfg_ind), soci::into(loc.storage_state),
         soci::use(entity_id));
    st.execute(false);
    while (st.fetch()) {
      if (mime_ind != soci::i_ok) {
        loc.mime_type.clear();
      }
      if (cfg_ind != soci::i_ok) {
        loc.storage_config_json = "{}";
      }
      out.push_back(loc);
    }
    return out;
  } catch (const soci::soci_error& e) {
    return translate("查询实体位置失败", e, entity_id);
  }
}

Result<std::map<std::string, std::string>> PgStore::lookup_locations(
    const std::string& storage_id, const std::vector<std::string>& keys) {
  std::map<std::string, std::string> out;
  if (keys.empty()) {
    return out;
  }

  constexpr std::size_t kBatch = 500;
  try {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    for (std::size_t begin = 0; begin < keys.size(); begin += kBatch) {
      const std::size_t end = std::min(keys.size(), begin + kBatch);
      Json::Value arr(Json::arrayValue);
      for (std::size_t i = begin; i < end; ++i) {
        arr.append(keys[i]);
      }
      const std::string json = Json::writeString(builder, arr);

      // jsonb 数组展开后走 IN，一条语句处理一批；key 里的引号/换行由 JSON 负责转义
      std::string key;
      std::string entity_id;
      soci::statement st =
          (db_->prepare << "select key, entity_id::text from entity_locations "
                          "where storage_id = CAST(:s AS uuid) "
                          "and key in (select jsonb_array_elements_text(CAST(:ks AS jsonb)))",
           soci::into(key), soci::into(entity_id), soci::use(storage_id), soci::use(json));
      st.execute(false);
      while (st.fetch()) {
        out.emplace(key, entity_id);
      }
    }
    return out;
  } catch (const soci::soci_error& e) {
    return translate("批量查询实体位置失败", e, storage_id);
  }
}

Status PgStore::mark_location_ok(const std::string& entity_id, const std::string& storage_id,
                                 const std::string& key) {
  try {
    *db_ << "update entity_locations set failed_at = null "
            "where entity_id = CAST(:e AS uuid) and storage_id = CAST(:s AS uuid) and key = :k",
        soci::use(entity_id), soci::use(storage_id), soci::use(key);
    return {};
  } catch (const soci::soci_error& e) {
    return translate("清空 entity_locations.failed_at 失败", e, key);
  }
}

Status PgStore::mark_location_failed(const std::string& entity_id, const std::string& storage_id,
                                     const std::string& key) {
  try {
    *db_ << "update entity_locations set failed_at = now() "
            "where entity_id = CAST(:e AS uuid) and storage_id = CAST(:s AS uuid) and key = :k",
        soci::use(entity_id), soci::use(storage_id), soci::use(key);
    return {};
  } catch (const soci::soci_error& e) {
    return translate("写入 entity_locations.failed_at 失败", e, key);
  }
}

} // namespace sb
