#include "result.hpp"

namespace sb {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::InvalidArgument:
    return "invalid_argument";
  case ErrorCode::Config:
    return "config";
  case ErrorCode::NotFound:
    return "not_found";
  case ErrorCode::Busy:
    return "busy";
  case ErrorCode::Conflict:
    return "conflict";
  case ErrorCode::Auth:
    return "auth";
  case ErrorCode::Network:
    return "network";
  case ErrorCode::Storage:
    return "storage";
  case ErrorCode::Postgres:
    return "postgres";
  case ErrorCode::Io:
    return "io";
  case ErrorCode::Internal:
    return "internal";
  }
  return "unknown";
}

std::string Error::to_string() const {
  std::string out = std::string("[") + sb::to_string(code) + "] " + message;
  if (!detail.empty()) {
    out += " (" + detail + ")";
  }
  if (!context.empty()) {
    out += " @ " + context;
  }
  return out;
}

} // namespace sb
