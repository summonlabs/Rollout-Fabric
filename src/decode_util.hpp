// Rollout Fabric - internal decode helpers.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Decoding is repetitive and every field must be bounds checked with a
// diagnostic that names the offending field. These helpers keep that checking
// uniform instead of letting each translation unit invent its own. This header
// is private to the library: it is not installed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rollout_fabric/codec.hpp"
#include "rollout_fabric/digest.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"

// Runs an expression returning Status and returns early on failure.
#define RF_TRY(expression)                    \
  do {                                        \
    const ::rollout_fabric::Status rf_status_ = (expression); \
    if (!rf_status_.ok()) {                   \
      return rf_status_;                      \
    }                                         \
  } while (false)

// Runs an expression returning Result<T>; on failure returns the status, on
// success assigns to the target.
#define RF_TRY_ASSIGN(expression, target)     \
  do {                                        \
    auto rf_result_ = (expression);           \
    if (!rf_result_.ok()) {                   \
      return rf_result_.status();             \
    }                                         \
    (target) = rf_result_.value();            \
  } while (false)

namespace rollout_fabric::detail {

inline Status dc_u8(ByteReader& reader, const char* field, std::uint8_t& out) {
  auto value = reader.u8();
  if (!value.ok()) {
    return make_status(value.status().code(), std::string(field) + ": " + value.status().message());
  }
  out = value.value();
  return Status::success();
}

inline Status dc_u16(ByteReader& reader, const char* field, std::uint16_t& out) {
  auto value = reader.u16();
  if (!value.ok()) {
    return make_status(value.status().code(), std::string(field) + ": " + value.status().message());
  }
  out = value.value();
  return Status::success();
}

inline Status dc_u32(ByteReader& reader, const char* field, std::uint32_t& out) {
  auto value = reader.u32();
  if (!value.ok()) {
    return make_status(value.status().code(), std::string(field) + ": " + value.status().message());
  }
  out = value.value();
  return Status::success();
}

inline Status dc_u64(ByteReader& reader, const char* field, std::uint64_t& out) {
  auto value = reader.u64();
  if (!value.ok()) {
    return make_status(value.status().code(), std::string(field) + ": " + value.status().message());
  }
  out = value.value();
  return Status::success();
}

inline Status dc_i64(ByteReader& reader, const char* field, std::int64_t& out) {
  auto value = reader.i64();
  if (!value.ok()) {
    return make_status(value.status().code(), std::string(field) + ": " + value.status().message());
  }
  out = value.value();
  return Status::success();
}

inline Status dc_bool(ByteReader& reader, const char* field, bool& out) {
  auto value = reader.boolean();
  if (!value.ok()) {
    return make_status(value.status().code(), std::string(field) + ": " + value.status().message());
  }
  out = value.value();
  return Status::success();
}

inline Status dc_duration(ByteReader& reader, const char* field, Duration& out) {
  std::int64_t nanos = 0;
  RF_TRY(dc_i64(reader, field, nanos));
  out = Duration::from_nanos(nanos);
  return Status::success();
}

inline Status dc_timestamp(ByteReader& reader, const char* field, Timestamp& out) {
  std::int64_t nanos = 0;
  RF_TRY(dc_i64(reader, field, nanos));
  out = Timestamp::from_unix_nanos(nanos);
  return Status::success();
}

inline Status dc_string(ByteReader& reader, const char* field, std::size_t max_length,
                        std::string& out) {
  auto value = reader.string(max_length);
  if (!value.ok()) {
    return make_status(value.status().code(), std::string(field) + ": " + value.status().message());
  }
  out = std::move(value).value();
  return Status::success();
}

inline Status dc_digest(ByteReader& reader, const char* field, Digest256& out) {
  auto value = reader.raw(kDigest256Bytes);
  if (!value.ok()) {
    return make_status(value.status().code(), std::string(field) + ": " + value.status().message());
  }
  for (std::size_t index = 0; index < kDigest256Bytes; ++index) {
    out[index] = value.value()[index];
  }
  return Status::success();
}

inline Status dc_bytes(ByteReader& reader, const char* field, std::size_t max_length,
                       std::vector<std::byte>& out) {
  auto value = reader.bytes(max_length);
  if (!value.ok()) {
    return make_status(value.status().code(), std::string(field) + ": " + value.status().message());
  }
  out.assign(value.value().begin(), value.value().end());
  return Status::success();
}

template <class Id>
inline Status dc_id(ByteReader& reader, const char* field, Id& out) {
  auto value = reader.raw(Id::byte_size);
  if (!value.ok()) {
    return make_status(value.status().code(), std::string(field) + ": " + value.status().message());
  }
  out = Id::from_bytes(std::span<const std::byte, Id::byte_size>(value.value().data(), Id::byte_size));
  return Status::success();
}

template <class Id>
inline Status dc_id_vector(ByteReader& reader, const char* field, std::size_t max_count,
                           std::vector<Id>& out) {
  std::uint32_t count = 0;
  RF_TRY(dc_u32(reader, field, count));
  if (count > max_count) {
    return make_status(StatusCode::kLimitExceeded,
                       std::string(field) + ": declared count " + std::to_string(count) +
                           " exceeds the maximum " + std::to_string(max_count));
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    Id value{};
    RF_TRY(dc_id(reader, field, value));
    out.push_back(value);
  }
  return Status::success();
}

inline Status dc_string_vector(ByteReader& reader, const char* field, std::size_t max_count,
                               std::size_t max_length, std::vector<std::string>& out) {
  std::uint32_t count = 0;
  RF_TRY(dc_u32(reader, field, count));
  if (count > max_count) {
    return make_status(StatusCode::kLimitExceeded,
                       std::string(field) + ": declared count " + std::to_string(count) +
                           " exceeds the maximum " + std::to_string(max_count));
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string value;
    RF_TRY(dc_string(reader, field, max_length, value));
    out.push_back(std::move(value));
  }
  return Status::success();
}

inline void wr_duration(ByteWriter& writer, Duration value) { writer.i64(value.nanos()); }
inline void wr_timestamp(ByteWriter& writer, Timestamp value) { writer.i64(value.unix_nanos()); }
inline void wr_digest(ByteWriter& writer, const Digest256& value) { writer.raw(value); }

template <class Id>
inline void wr_id(ByteWriter& writer, const Id& value) {
  writer.raw(value.span());
}

}  // namespace rollout_fabric::detail
