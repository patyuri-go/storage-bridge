#pragma once

#include "result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace sb {

// 流式 SHA-256。用 OpenSSL EVP，便于以后换成 sha1/blake2 只改一行。
class Sha256 {
public:
  Sha256();
  ~Sha256();

  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  Status update(const void* data, std::size_t len);
  Result<std::string> hex_final(); // 调用后不可再 update

private:
  void* ctx_ = nullptr;
  bool finalized_ = false;
};

} // namespace sb
