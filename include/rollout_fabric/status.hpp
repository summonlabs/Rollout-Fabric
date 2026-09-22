// Rollout Fabric - status codes and result type.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rollout_fabric {

// Stable numeric codes. Persisted and transported, so the numbering is part of
// the compatibility surface and values are never reused.
enum class StatusCode : std::uint16_t {
  kOk = 0,
  kInvalidArgument = 1,
  kNotFound = 2,
  kAlreadyExists = 3,
  kConflict = 4,
  kPreconditionFailed = 5,
  kPermissionDenied = 6,
  kResourceExhausted = 7,
  kUnavailable = 8,
  kInternal = 9,
  kCorrupt = 10,
  // Fencing family. Each identifies a different stale-authority path so that a
  // caller can distinguish "your generation is old" from "your attempt is old".
  kStaleEpoch = 11,
  kStaleGeneration = 12,
  kStaleAttempt = 13,
  kStaleEvidence = 14,
  kStaleRevision = 15,
  kFenced = 16,
  kNotSupported = 17,
  kCancelled = 18,
  kShuttingDown = 19,
  kLimitExceeded = 20,
  kPolicyViolation = 21,
  kDeadlineExceeded = 22,
  kIoError = 23,
  kLocked = 24,
  kImmutable = 25,
  kUnsupportedGate = 26,
};

[[nodiscard]] std::string_view to_string(StatusCode code) noexcept;

// A status is a code plus a bounded human readable explanation. The text is
// diagnostic only; no decision ever depends on it.
class Status {
 public:
  Status() noexcept = default;
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}
  explicit Status(StatusCode code) : code_(code) {}

  // Named success() rather than ok() so that it cannot be confused with the
  // instance predicate of the same meaning.
  [[nodiscard]] static Status success() noexcept { return Status(); }

  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::kOk; }
  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }

  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Status& lhs, const Status& rhs) noexcept {
    return lhs.code_ == rhs.code_ && lhs.message_ == rhs.message_;
  }

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

// Builds a Status with a message.
[[nodiscard]] Status make_status(StatusCode code, std::string message);

// Result<T> carries either a value or a Status. Callers must check ok() before
// reading value(); value() on a failed result is a programming error and
// throws std::bad_optional_access rather than returning a default value.
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}
  Result(Status status) : status_(std::move(status)) {
    if (status_.ok()) {
      status_ = make_status(StatusCode::kInternal, "Result constructed from a non-error status");
    }
  }

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

  [[nodiscard]] const T& value() const& { return value_.value(); }
  [[nodiscard]] T& value() & { return value_.value(); }
  [[nodiscard]] T&& value() && { return std::move(value_).value(); }

  [[nodiscard]] const T& operator*() const& { return value_.value(); }
  [[nodiscard]] T& operator*() & { return value_.value(); }
  [[nodiscard]] T* operator->() { return &value_.value(); }
  [[nodiscard]] const T* operator->() const { return &value_.value(); }

 private:
  std::optional<T> value_;
  Status status_ = Status::success();
};

}  // namespace rollout_fabric
