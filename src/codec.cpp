// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/codec.hpp"

#include <cstring>

namespace rollout_fabric {
namespace {

struct U128 {
  std::uint64_t high = 0;
  std::uint64_t low = 0;
};

// Full 64x64 -> 128 bit product using 32 bit limbs.
[[nodiscard]] U128 multiply_wide(std::uint64_t lhs, std::uint64_t rhs) noexcept {
  const std::uint64_t lhs_low = lhs & 0xFFFFFFFFull;
  const std::uint64_t lhs_high = lhs >> 32;
  const std::uint64_t rhs_low = rhs & 0xFFFFFFFFull;
  const std::uint64_t rhs_high = rhs >> 32;

  const std::uint64_t p0 = lhs_low * rhs_low;
  const std::uint64_t p1 = lhs_low * rhs_high;
  const std::uint64_t p2 = lhs_high * rhs_low;
  const std::uint64_t p3 = lhs_high * rhs_high;

  const std::uint64_t middle = (p0 >> 32) + (p1 & 0xFFFFFFFFull) + (p2 & 0xFFFFFFFFull);
  U128 result;
  result.low = (p0 & 0xFFFFFFFFull) | (middle << 32);
  result.high = p3 + (p1 >> 32) + (p2 >> 32) + (middle >> 32);
  return result;
}

[[nodiscard]] int compare_wide(const U128& lhs, const U128& rhs) noexcept {
  if (lhs.high != rhs.high) {
    return lhs.high < rhs.high ? -1 : 1;
  }
  if (lhs.low != rhs.low) {
    return lhs.low < rhs.low ? -1 : 1;
  }
  return 0;
}

}  // namespace

bool checked_add(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& out) noexcept {
  if (lhs > UINT64_MAX - rhs) {
    return false;
  }
  out = lhs + rhs;
  return true;
}

bool checked_mul(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& out) noexcept {
  if (lhs == 0 || rhs == 0) {
    out = 0;
    return true;
  }
  if (lhs > UINT64_MAX / rhs) {
    return false;
  }
  out = lhs * rhs;
  return true;
}

int compare_products(std::uint64_t lhs_a,
                     std::uint64_t lhs_b,
                     std::uint64_t rhs_a,
                     std::uint64_t rhs_b) noexcept {
  return compare_wide(multiply_wide(lhs_a, lhs_b), multiply_wide(rhs_a, rhs_b));
}

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  const std::size_t size = text.size();
  while (index < size) {
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80u) {
      ++index;
      continue;
    }
    std::size_t continuation = 0;
    std::uint32_t codepoint = 0;
    std::uint32_t minimum = 0;
    if ((lead & 0xE0u) == 0xC0u) {
      continuation = 1;
      codepoint = lead & 0x1Fu;
      minimum = 0x80u;
    } else if ((lead & 0xF0u) == 0xE0u) {
      continuation = 2;
      codepoint = lead & 0x0Fu;
      minimum = 0x800u;
    } else if ((lead & 0xF8u) == 0xF0u) {
      continuation = 3;
      codepoint = lead & 0x07u;
      minimum = 0x10000u;
    } else {
      return false;
    }
    if (size - index - 1 < continuation) {
      return false;  // truncated multi-byte sequence
    }
    for (std::size_t offset = 1; offset <= continuation; ++offset) {
      const auto next = static_cast<unsigned char>(text[index + offset]);
      if ((next & 0xC0u) != 0x80u) {
        return false;
      }
      codepoint = (codepoint << 6) | (next & 0x3Fu);
    }
    if (codepoint < minimum) {
      return false;  // overlong encoding
    }
    if (codepoint > 0x10FFFFu) {
      return false;
    }
    if (codepoint >= 0xD800u && codepoint <= 0xDFFFu) {
      return false;  // surrogate half
    }
    index += continuation + 1;
  }
  return true;
}

void ByteWriter::u8(std::uint8_t value) {
  out_.push_back(static_cast<std::byte>(value));
}

void ByteWriter::u16(std::uint16_t value) {
  out_.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(value & 0xFFu)));
  out_.push_back(static_cast<std::byte>(static_cast<std::uint8_t>((value >> 8) & 0xFFu)));
}

void ByteWriter::u32(std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out_.push_back(static_cast<std::byte>(static_cast<std::uint8_t>((value >> shift) & 0xFFu)));
  }
}

void ByteWriter::u64(std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out_.push_back(static_cast<std::byte>(static_cast<std::uint8_t>((value >> shift) & 0xFFu)));
  }
}

void ByteWriter::i64(std::int64_t value) {
  u64(static_cast<std::uint64_t>(value));
}

void ByteWriter::boolean(bool value) {
  u8(value ? 1u : 0u);
}

void ByteWriter::raw(std::span<const std::byte> payload) {
  out_.insert(out_.end(), payload.begin(), payload.end());
}

void ByteWriter::bytes(std::span<const std::byte> payload) {
  u32(static_cast<std::uint32_t>(payload.size()));
  raw(payload);
}

void ByteWriter::string(std::string_view text) {
  u32(static_cast<std::uint32_t>(text.size()));
  raw(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

void ByteWriter::presence(bool present) {
  boolean(present);
}

Result<std::span<const std::byte>> ByteReader::take(std::size_t count) {
  if (count > remaining()) {
    return make_status(StatusCode::kCorrupt, "truncated input: requested " + std::to_string(count) +
                                                 " bytes with " + std::to_string(remaining()) +
                                                 " remaining");
  }
  const std::span<const std::byte> slice = input_.subspan(position_, count);
  position_ += count;
  return slice;
}

Result<std::uint8_t> ByteReader::u8() {
  auto slice = take(1);
  if (!slice.ok()) {
    return slice.status();
  }
  return std::to_integer<std::uint8_t>(slice.value()[0]);
}

Result<std::uint16_t> ByteReader::u16() {
  auto slice = take(2);
  if (!slice.ok()) {
    return slice.status();
  }
  const auto* data = slice.value().data();
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(data[0]) |
      (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(data[1])) << 8));
}

Result<std::uint32_t> ByteReader::u32() {
  auto slice = take(4);
  if (!slice.ok()) {
    return slice.status();
  }
  const auto* data = slice.value().data();
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[index])) << (index * 8);
  }
  return value;
}

Result<std::uint64_t> ByteReader::u64() {
  auto slice = take(8);
  if (!slice.ok()) {
    return slice.status();
  }
  const auto* data = slice.value().data();
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data[index])) << (index * 8);
  }
  return value;
}

Result<std::int64_t> ByteReader::i64() {
  auto value = u64();
  if (!value.ok()) {
    return value.status();
  }
  return static_cast<std::int64_t>(value.value());
}

Result<bool> ByteReader::boolean() {
  auto value = u8();
  if (!value.ok()) {
    return value.status();
  }
  if (value.value() > 1u) {
    return make_status(StatusCode::kCorrupt,
                       "invalid boolean encoding: " + std::to_string(value.value()));
  }
  return value.value() == 1u;
}

Result<std::span<const std::byte>> ByteReader::raw(std::size_t count) { return take(count); }

Result<std::span<const std::byte>> ByteReader::bytes(std::size_t max_length) {
  auto length = u32();
  if (!length.ok()) {
    return length.status();
  }
  if (length.value() > max_length) {
    return make_status(StatusCode::kLimitExceeded,
                       "declared length " + std::to_string(length.value()) +
                           " exceeds the configured maximum " + std::to_string(max_length));
  }
  return take(length.value());
}

Result<std::vector<std::byte>> ByteReader::owned_bytes(std::size_t max_length) {
  auto slice = bytes(max_length);
  if (!slice.ok()) {
    return slice.status();
  }
  return std::vector<std::byte>(slice.value().begin(), slice.value().end());
}

Result<std::string> ByteReader::string(std::size_t max_length) {
  auto slice = bytes(max_length);
  if (!slice.ok()) {
    return slice.status();
  }
  std::string text(reinterpret_cast<const char*>(slice.value().data()), slice.value().size());
  if (!is_valid_utf8(text)) {
    return make_status(StatusCode::kCorrupt, "string field is not valid UTF-8");
  }
  return text;
}

Result<bool> ByteReader::presence() { return boolean(); }

Status ByteReader::expect_end() const {
  if (!at_end()) {
    return make_status(StatusCode::kCorrupt,
                       std::to_string(remaining()) + " trailing byte(s) after the final field");
  }
  return Status::success();
}

}  // namespace rollout_fabric
