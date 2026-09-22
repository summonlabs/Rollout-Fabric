// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/digest.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace rollout_fabric {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::array<std::uint32_t, 8> kInitialState = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                                        0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                                        0x1f83d9abu, 0x5be0cd19u};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, unsigned count) noexcept {
  return (value >> count) | (value << (32u - count));
}

[[nodiscard]] constexpr std::uint32_t big_endian_load(const std::byte* data) noexcept {
  return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[0])) << 24) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[1])) << 16) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[2])) << 8) |
         static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[3]));
}

constexpr std::array<std::uint32_t, 256> make_crc_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256; ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1u) != 0u ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
    }
    table[index] = value;
  }
  return table;
}

constexpr auto kCrcTable = make_crc_table();

[[nodiscard]] constexpr char hex_digit(unsigned value) noexcept {
  return static_cast<char>(value < 10 ? ('0' + value) : ('a' + (value - 10)));
}

[[nodiscard]] constexpr int hex_value(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return 10 + (character - 'a');
  }
  if (character >= 'A' && character <= 'F') {
    return 10 + (character - 'A');
  }
  return -1;
}

}  // namespace

Sha256::Sha256() noexcept { reset(); }

void Sha256::reset() noexcept {
  state_ = kInitialState;
  buffer_.fill(std::byte{0});
  buffered_ = 0;
  total_bytes_ = 0;
  finalized_ = false;
}

void Sha256::compress() noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    schedule[index] = big_endian_load(buffer_.data() + (index * 4));
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotr(schedule[index - 15], 7) ^ rotr(schedule[index - 15], 18) ^
                             (schedule[index - 15] >> 3);
    const std::uint32_t s1 = rotr(schedule[index - 2], 17) ^ rotr(schedule[index - 2], 19) ^
                             (schedule[index - 2] >> 10);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t sigma1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + sigma1 + choose + kRoundConstants[index] + schedule[index];
    const std::uint32_t sigma0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = sigma0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::byte> data) noexcept {
  if (finalized_) {
    reset();
  }
  total_bytes_ += static_cast<std::uint64_t>(data.size());
  std::size_t offset = 0;
  if (buffered_ != 0) {
    const std::size_t want = std::min<std::size_t>(64 - buffered_, data.size());
    std::memcpy(buffer_.data() + buffered_, data.data(), want);
    buffered_ += want;
    offset = want;
    if (buffered_ == 64) {
      compress();
      buffered_ = 0;
    }
  }
  while (offset + 64 <= data.size()) {
    std::memcpy(buffer_.data(), data.data() + offset, 64);
    compress();
    offset += 64;
  }
  if (offset < data.size()) {
    const std::size_t remaining = data.size() - offset;
    std::memcpy(buffer_.data(), data.data() + offset, remaining);
    buffered_ = remaining;
  }
}

void Sha256::update(std::string_view text) noexcept {
  update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

void Sha256::update(std::uint8_t byte) noexcept {
  const std::byte single = static_cast<std::byte>(byte);
  update(std::span<const std::byte>(&single, 1));
}

Digest256 Sha256::peek() const noexcept {
  Sha256 copy = *this;
  return copy.finish();
}

Digest256 Sha256::finish() noexcept {
  const std::uint64_t bit_length = total_bytes_ * 8u;

  std::array<std::byte, 8> length_bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    length_bytes[index] = static_cast<std::byte>(
        static_cast<std::uint8_t>((bit_length >> ((7u - index) * 8u)) & 0xFFu));
  }

  const std::byte marker = static_cast<std::byte>(0x80);
  update(std::span<const std::byte>(&marker, 1));
  const std::byte zero = std::byte{0};
  while (buffered_ != 56) {
    update(std::span<const std::byte>(&zero, 1));
  }
  // update() above incremented total_bytes_; the length field must describe the
  // message, not the padding.
  std::memcpy(buffer_.data() + 56, length_bytes.data(), 8);
  compress();
  buffered_ = 0;

  Digest256 out{};
  for (std::size_t index = 0; index < 8; ++index) {
    out[index * 4 + 0] = static_cast<std::byte>(static_cast<std::uint8_t>((state_[index] >> 24) & 0xFFu));
    out[index * 4 + 1] = static_cast<std::byte>(static_cast<std::uint8_t>((state_[index] >> 16) & 0xFFu));
    out[index * 4 + 2] = static_cast<std::byte>(static_cast<std::uint8_t>((state_[index] >> 8) & 0xFFu));
    out[index * 4 + 3] = static_cast<std::byte>(static_cast<std::uint8_t>(state_[index] & 0xFFu));
  }
  finalized_ = true;
  return out;
}

Digest256 sha256(std::span<const std::byte> data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finish();
}

Digest256 sha256(std::string_view text) noexcept {
  Sha256 hasher;
  hasher.update(text);
  return hasher.finish();
}

Digest128 truncate128(const Digest256& in) noexcept {
  Digest128 out{};
  std::copy(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(kDigest128Bytes), out.begin());
  return out;
}

Digest256 widen128(const Digest128& in) noexcept {
  Digest256 out{};
  std::copy(in.begin(), in.end(), out.begin());
  return out;
}

std::uint32_t crc32(std::span<const std::byte> data, std::uint32_t seed) noexcept {
  std::uint32_t crc = seed ^ 0xFFFFFFFFu;
  for (const std::byte byte : data) {
    crc = kCrcTable[(crc ^ std::to_integer<std::uint8_t>(byte)) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

std::string to_hex(std::span<const std::byte> bytes) {
  std::string out;
  out.resize(bytes.size() * 2);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const auto value = std::to_integer<std::uint8_t>(bytes[index]);
    out[index * 2] = hex_digit(static_cast<unsigned>(value >> 4));
    out[index * 2 + 1] = hex_digit(static_cast<unsigned>(value & 0x0Fu));
  }
  return out;
}

std::string to_hex(const Digest256& digest) {
  return to_hex(std::span<const std::byte>(digest.data(), digest.size()));
}

std::string to_hex(const Digest128& digest) {
  return to_hex(std::span<const std::byte>(digest.data(), digest.size()));
}

bool parse_hex(std::string_view text, std::span<std::byte> out) noexcept {
  if (text.size() != out.size() * 2) {
    return false;
  }
  for (std::size_t index = 0; index < out.size(); ++index) {
    const int high = hex_value(text[index * 2]);
    const int low = hex_value(text[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    out[index] = static_cast<std::byte>(static_cast<std::uint8_t>((high << 4) | low));
  }
  return true;
}

bool parse_hex128(std::string_view text, Digest128& out) noexcept {
  return parse_hex(text, std::span<std::byte>(out.data(), out.size()));
}

bool parse_hex256(std::string_view text, Digest256& out) noexcept {
  return parse_hex(text, std::span<std::byte>(out.data(), out.size()));
}

bool digest_equal(std::span<const std::byte> lhs, std::span<const std::byte> rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  unsigned char difference = 0;
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    difference |= static_cast<unsigned char>(
        std::to_integer<std::uint8_t>(lhs[index]) ^ std::to_integer<std::uint8_t>(rhs[index]));
  }
  return difference == 0;
}

}  // namespace rollout_fabric
