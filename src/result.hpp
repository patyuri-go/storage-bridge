#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace sb {

// 错误分类：既能生成日志，也能映射 HTTP 状态码 / 进程退出码
enum class ErrorCode {
  InvalidArgument, // 参数/输入不合法
  Config,          // 配置文件问题
  NotFound,        // 对象/记录不存在
  Conflict,        // 唯一约束冲突
  Auth,            // 认证/授权失败
  Network,         // 网络错误（DNS/连接/超时）
  Storage,         // 存储端返回的错误（HTTP 4xx/5xx、S3 错误码）
  Postgres,        // 远端 PostgreSQL
  Io,              // 本地文件读写
  Busy,            // 资源暂时不足，可重试（如任务队列已满）
  Internal,        // 其它未分类
};

const char* to_string(ErrorCode code) noexcept;

// 自定义错误结构体：code 决定处理策略，message 给人看，detail/context 给排查用
struct Error {
  ErrorCode code = ErrorCode::Internal;
  std::string message;
  std::string detail;  // SQLSTATE、HTTP 状态、davix 错误码等
  std::string context; // URI、配置键、表名等

  std::string to_string() const;
};

// 构造帮助函数：err(ErrorCode::Config, "缺少 database 块", "", path)
inline Error err(ErrorCode code, std::string message, std::string detail = {},
                 std::string context = {}) {
  Error e;
  e.code = code;
  e.message = std::move(message);
  e.detail = std::move(detail);
  e.context = std::move(context);
  return e;
}

// 成功/失败判定
template <typename T, typename E = Error> class Result {
  static_assert(!std::is_void_v<T>, "Result<void> 请使用特化版本");

public:
  using value_type = T;
  using error_type = E;

  Result(T value)
      : data_(std::in_place_index<0>, std::move(value)) {} // NOLINT(google-explicit-constructor)
  Result(E error)
      : data_(std::in_place_index<1>, std::move(error)) {} // NOLINT(google-explicit-constructor)

  bool ok() const noexcept {
    return data_.index() == 0;
  }
  explicit operator bool() const noexcept {
    return ok();
  }

  T& value() & {
    assert(ok());
    return std::get<0>(data_);
  }
  const T& value() const& {
    assert(ok());
    return std::get<0>(data_);
  }
  T&& value() && {
    assert(ok());
    return std::get<0>(std::move(data_));
  }
  T& operator*() & {
    return value();
  }
  const T& operator*() const& {
    return value();
  }
  T* operator->() {
    return &value();
  }
  const T* operator->() const {
    return &value();
  }

  E& error() & {
    assert(!ok());
    return std::get<1>(data_);
  }
  const E& error() const& {
    assert(!ok());
    return std::get<1>(data_);
  }

  // 取值失败时返回兜底值（不吞错，调用方仍可用 ok() 判断）
  T value_or(T fallback) const& {
    return ok() ? value() : std::move(fallback);
  }

private:
  std::variant<T, E> data_;
};

// Result<void>：只关心成功/失败
template <typename E> class Result<void, E> {
public:
  using value_type = void;
  using error_type = E;

  Result() noexcept = default;
  Result(E error) : error_(std::move(error)) {} // NOLINT(google-explicit-constructor)

  bool ok() const noexcept {
    return !error_.has_value();
  }
  explicit operator bool() const noexcept {
    return ok();
  }

  E& error() & {
    assert(!ok());
    return *error_;
  }
  const E& error() const& {
    assert(!ok());
    return *error_;
  }

private:
  std::optional<E> error_;
};

using Status = Result<void, Error>;

} // namespace sb
