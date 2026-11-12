#include "hasher.hpp"

#include <openssl/evp.h>

#include <cstring>

namespace sb {

namespace {
const char* kHex = "0123456789abcdef";
}

Sha256::Sha256() {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(ctx);
    // 初始化失败时 ctx_ 保持 nullptr，后续 update/hex_final 返回 Internal 错误
    return;
  }
  ctx_ = ctx;
}

Sha256::~Sha256() {
  EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx_));
}

Status Sha256::update(const void* data, std::size_t len) {
  if (ctx_ == nullptr) {
    return err(ErrorCode::Internal, "SHA-256 上下文未初始化");
  }
  if (finalized_) {
    return err(ErrorCode::Internal, "Sha256::update 在 hex_final 之后被调用");
  }
  if (len == 0) {
    return {};
  }
  if (EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(ctx_), data, len) != 1) {
    return err(ErrorCode::Internal, "EVP_DigestUpdate 失败");
  }
  return {};
}

Result<std::string> Sha256::hex_final() {
  if (ctx_ == nullptr) {
    return err(ErrorCode::Internal, "SHA-256 上下文未初始化");
  }
  if (finalized_) {
    return err(ErrorCode::Internal, "Sha256::hex_final 被调用了两次");
  }
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  if (EVP_DigestFinal_ex(static_cast<EVP_MD_CTX*>(ctx_), md, &len) != 1) {
    return err(ErrorCode::Internal, "EVP_DigestFinal_ex 失败");
  }
  finalized_ = true;

  std::string out;
  out.reserve(static_cast<std::size_t>(len) * 2);
  for (unsigned int i = 0; i < len; ++i) {
    out.push_back(kHex[md[i] >> 4]);
    out.push_back(kHex[md[i] & 0x0F]);
  }
  return out;
}

} // namespace sb
