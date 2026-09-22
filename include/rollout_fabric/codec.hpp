// Rollout Fabric - canonical bounded binary encoding.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every structure this runtime persists or transports has exactly one canonical
// byte representation. Digests, journal records and wire frames all derive from
// it, so "the same logical value" always means "the same bytes". Readers are
// bounds checked and take an explicit bound for every variable length field;
// nothing in this header trusts a length that arrived from outside.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rollout_fabric/status.hpp"

namespace rollout_fabric {

// Checked size arithmetic for externally derived lengths.
[[nodiscard]] bool checked_add(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& out) noexcept;
[[nodiscard]] bool checked_mul(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& out) noexcept;

// Sign of (lhs_a * lhs_b) - (rhs_a * rhs_b) computed without overflowing:
// returns -1, 0 or 1. Ratio and failure-budget comparisons are all expressed
// through this so that no externally derived count can wrap.
[[nodiscard]] int compare_products(std::uint64_t lhs_a,
                                   std::uint64_t lhs_b,
                                   std::uint64_t rhs_a,
                                   std::uint64_t rhs_b) noexcept;

// True when the UTF-8 text is structurally valid (rejects overlong forms,
// surrogates and out-of-range leads). Identifiers and labels that arrive from
// operators pass through here before they can be persisted or echoed.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

class ByteWriter {
 public:
  ByteWriter() = default;

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void boolean(bool value);
  void raw(std::span<const std::byte> payload);
  void bytes(std::span<const std::byte> payload);
  void string(std::string_view text);
  void presence(bool present);

  [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }
  [[nodiscard]] bool empty() const noexcept { return out_.empty(); }
  [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return out_; }
  [[nodiscard]] std::span<const std::byte> span() const noexcept {
    return std::span<const std::byte>(out_.data(), out_.size());
  }
  void clear() noexcept { out_.clear(); }
  void reserve(std::size_t bytes) { out_.reserve(bytes); }

 private:
  std::vector<std::byte> out_;
};

class ByteReader {
 public:
  ByteReader() = default;
  explicit ByteReader(std::span<const std::byte> input) noexcept : input_(input) {}

  [[nodiscard]] Result<std::uint8_t> u8();
  [[nodiscard]] Result<std::uint16_t> u16();
  [[nodiscard]] Result<std::uint32_t> u32();
  [[nodiscard]] Result<std::uint64_t> u64();
  [[nodiscard]] Result<std::int64_t> i64();
  [[nodiscard]] Result<bool> boolean();
  [[nodiscard]] Result<std::span<const std::byte>> raw(std::size_t count);
  [[nodiscard]] Result<std::span<const std::byte>> bytes(std::size_t max_length);
  [[nodiscard]] Result<std::vector<std::byte>> owned_bytes(std::size_t max_length);
  [[nodiscard]] Result<std::string> string(std::size_t max_length);
  [[nodiscard]] Result<bool> presence();

  [[nodiscard]] bool at_end() const noexcept { return position_ == input_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return input_.size() - position_; }
  [[nodiscard]] std::size_t position() const noexcept { return position_; }

  // Fails unless every byte was consumed. Used to reject trailing garbage that
  // would otherwise be silently ignored.
  [[nodiscard]] Status expect_end() const;

 private:
  [[nodiscard]] Result<std::span<const std::byte>> take(std::size_t count);

  std::span<const std::byte> input_{};
  std::size_t position_ = 0;
};

}  // namespace rollout_fabric
