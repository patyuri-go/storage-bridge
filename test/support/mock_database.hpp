#pragma once

#include "pg_store.hpp"

#include <gmock/gmock.h>

#include <map>
#include <string>
#include <vector>

namespace sb::test {

class MockDatabase : public IDatabase {
public:
  MOCK_METHOD(Status, ping, (), (override));

  MOCK_METHOD(Result<StorageNodeResult>, upsert_storage, (const StorageNodeInput&), (override));
  MOCK_METHOD(Result<StorageNodeRecord>, get_storage, (const std::string&), (override));
  MOCK_METHOD(Result<std::vector<StorageNodeRecord>>, list_storages, (), (override));

  MOCK_METHOD(Result<FileRecordOutcome>, record_file,
              (const std::string&, const std::string&, const std::string&, long long,
               const std::string&),
              (override));

  MOCK_METHOD(Result<bool>, location_matches_size,
              (const std::string&, const std::string&, long long), (override));

  MOCK_METHOD(Result<std::vector<EntityLocation>>, list_entity_locations,
              (const std::string&), (override));

  MOCK_METHOD((Result<std::map<std::string, std::string>>), lookup_locations,
              (const std::string&, const std::vector<std::string>&), (override));

  MOCK_METHOD(Status, mark_location_ok,
              (const std::string&, const std::string&, const std::string&), (override));
  MOCK_METHOD(Status, mark_location_failed,
              (const std::string&, const std::string&, const std::string&), (override));
};

} // namespace sb::test