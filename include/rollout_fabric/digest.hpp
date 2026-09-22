// Rollout Fabric - SHA-256 digests and CRC-32 record integrity.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace rollout_fabric {

inline constexpr std::size_t kDigest256Bytes = 32;
inline constexpr std::size_t kDigest128Bytes = 16;

using Digest256 = std::array<std::byte, kDigest256Bytes>;
using Digest128 = std::array<std::byte, kDigest128Bytes>;

// SHA-256. Incremental so that large canonical encodings can be hashed without
// materialising a second copy.
class Sha256 {
 public:
  Sha256() noexcept;

  void update(std::span<const std::byte> data) noexcept;
  void update(std::string_view text) noexcept;
  void update(std::uint8_t byte) noexcept;

  // Finalises without disturbing the running state's usability contract: the
  // object is reset and may be reused for the next message.
  [[nodiscard]] Digest256 finish() noexcept;

  // Non-destructive: the hasher remains usable afterwards.
  [[nodiscard]] Digest256 peek() const noexcept;

  void reset() noexcept;

 private:
  void compress() noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::byte, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
  bool finalized_ = false;
};

// One-shot helpers.
[[nodiscard]] Digest256 sha256(std::span<const std::byte> data) noexcept;
[[nodiscard]] Digest256 sha256(std::string_view text) noexcept;

// The first 16 bytes of a SHA-256. Used for transport frame integrity and
// identifier derivation, never for content addressing.
[[nodiscard]] Digest128 truncate128(const Digest256& in) noexcept;
[[nodiscard]] Digest256 widen128(const Digest128& in) noexcept;

// CRC-32 (IEEE 802.3 reflected polynomial), used for journal record integrity
// where bit-error detection with cheap verification matters more than
// collision resistance.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> data, std::uint32_t seed = 0) noexcept;

[[nodiscard]] std::string to_hex(std::span<const std::byte> bytes);
[[nodiscard]] std::string to_hex(const Digest256& digest);
[[nodiscard]] std::string to_hex(const Digest128& digest);

// Strict lowercase or uppercase hexadecimal. Rejects odd lengths, non-hex
// characters and length mismatches.
[[nodiscard]] bool parse_hex(std::string_view text, std::span<std::byte> out) noexcept;
[[nodiscard]] bool parse_hex128(std::string_view text, Digest128& out) noexcept;
[[nodiscard]] bool parse_hex256(std::string_view text, Digest256& out) noexcept;

// Constant-time comparison for digests used in equality checks on untrusted
// input.
[[nodiscard]] bool digest_equal(std::span<const std::byte> lhs, std::span<const std::byte> rhs) noexcept;

[[nodiscard]] constexpr bool is_nil(const Digest256& digest) noexcept {
  for (const std::byte b : digest) {
    if (b != std::byte{0}) {
      return false;
    }
  }
  return true;
}

}  // namespace rollout_fabric
