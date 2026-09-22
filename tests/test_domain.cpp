// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Unit tests for the Rollout Fabric value layer: SHA-256 and CRC-32 digests,
// hexadecimal and bounded binary codecs, UTF-8 validation, checked size
// arithmetic, strongly typed identifiers, timestamps, freshness, ratios and the
// lifecycle transition tables.
//
// Everything here is deterministic and self-contained: no sockets, no files and
// no reads of the system clock.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "rollout_fabric/codec.hpp"
#include "rollout_fabric/digest.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/lifecycle.hpp"
#include "rollout_fabric/policy.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"
#include "test_harness.hpp"

using namespace rollout_fabric;

namespace {

// ---------------------------------------------------------------------------
// Small helpers shared by the tests below.
// ---------------------------------------------------------------------------

[[nodiscard]] std::span<const std::byte> as_bytes(std::string_view text) noexcept {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

[[nodiscard]] std::string sha256_hex(std::string_view text) { return to_hex(sha256(text)); }

// Builds a std::string from explicit byte values so that the UTF-8 tests do not
// depend on how the compiler encodes non-ASCII literals.
[[nodiscard]] std::string utf8_bytes(std::initializer_list<unsigned int> values) {
  std::string out;
  out.reserve(values.size());
  for (const unsigned int value : values) {
    out.push_back(static_cast<char>(static_cast<unsigned char>(value)));
  }
  return out;
}

// Deterministic filler used for block-boundary hashing tests.
[[nodiscard]] std::string filler(std::size_t length) {
  std::string out;
  out.reserve(length);
  for (std::size_t position = 0; position < length; ++position) {
    const unsigned int value = static_cast<unsigned int>((position * 37u + 11u) % 251u);
    out.push_back(static_cast<char>(static_cast<unsigned char>(value)));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Lifecycle tables. The declared target lists are indexed by the enum value of
// the source state, so the index alignment is asserted before the tables are
// used.
// ---------------------------------------------------------------------------

template <class State>
[[nodiscard]] bool contains_state(const std::vector<State>& states, State state) {
  for (const State candidate : states) {
    if (candidate == state) {
      return true;
    }
  }
  return false;
}

template <class State>
void check_transition_table(const std::vector<State>& universe,
                            const std::vector<std::vector<State>>& declared) {
  RF_CHECK_EQ(universe.size(), declared.size());
  RF_REQUIRE(universe.size() == declared.size());
  for (std::size_t position = 0; position < universe.size(); ++position) {
    // The declared table is indexed by state value; a gap in the enum would
    // silently shift every row.
    RF_CHECK_EQ(static_cast<std::size_t>(universe[position]), position);
    // No state may transition to itself.
    RF_CHECK(!legal_transition(universe[position], universe[position]));
    // The declared row itself must be self-consistent.
    RF_CHECK(!contains_state(declared[position], universe[position]));
    for (std::size_t first = 0; first < declared[position].size(); ++first) {
      for (std::size_t second = first + 1; second < declared[position].size(); ++second) {
        RF_CHECK(!(declared[position][first] == declared[position][second]));
      }
    }
    // Every (from, to) pair in both directions must agree with the declaration.
    for (std::size_t target = 0; target < universe.size(); ++target) {
      const bool expected = contains_state(declared[position], universe[target]);
      const bool actual = legal_transition(universe[position], universe[target]);
      if (actual != expected) {
        RF_FAIL(std::string("legal_transition(") + std::string(to_string(universe[position])) + ", " +
                std::string(to_string(universe[target])) + ") was " + (actual ? "true" : "false") +
                ", the table declares " + (expected ? "true" : "false"));
      }
    }
  }
}

[[nodiscard]] const std::vector<RolloutState>& all_rollout_states() {
  static const std::vector<RolloutState> states = {
      RolloutState::kCreated,     RolloutState::kValidated, RolloutState::kArmed,
      RolloutState::kRunning,     RolloutState::kGated,     RolloutState::kSoaking,
      RolloutState::kAdvancing,   RolloutState::kPaused,    RolloutState::kAborting,
      RolloutState::kRollingBack, RolloutState::kFailed,    RolloutState::kCompleted,
      RolloutState::kRetired};
  return states;
}

[[nodiscard]] const std::vector<std::vector<RolloutState>>& rollout_table() {
  static const std::vector<std::vector<RolloutState>> table = {
      /* kCreated     */ {RolloutState::kValidated, RolloutState::kRetired},
      /* kValidated   */ {RolloutState::kArmed, RolloutState::kRetired},
      /* kArmed       */ {RolloutState::kRunning, RolloutState::kGated, RolloutState::kPaused,
                          RolloutState::kAborting, RolloutState::kFailed, RolloutState::kRetired},
      /* kRunning     */ {RolloutState::kGated, RolloutState::kSoaking, RolloutState::kAdvancing,
                          RolloutState::kPaused, RolloutState::kAborting, RolloutState::kRollingBack,
                          RolloutState::kFailed, RolloutState::kCompleted},
      /* kGated       */ {RolloutState::kRunning, RolloutState::kAdvancing, RolloutState::kPaused,
                          RolloutState::kAborting, RolloutState::kRollingBack, RolloutState::kFailed,
                          RolloutState::kCompleted},
      /* kSoaking     */ {RolloutState::kRunning, RolloutState::kAdvancing, RolloutState::kGated,
                          RolloutState::kPaused, RolloutState::kAborting, RolloutState::kRollingBack,
                          RolloutState::kFailed, RolloutState::kCompleted},
      /* kAdvancing   */ {RolloutState::kRunning, RolloutState::kGated, RolloutState::kPaused,
                          RolloutState::kAborting, RolloutState::kRollingBack, RolloutState::kFailed,
                          RolloutState::kCompleted},
      /* kPaused      */ {RolloutState::kArmed, RolloutState::kRunning, RolloutState::kGated,
                          RolloutState::kSoaking, RolloutState::kAdvancing, RolloutState::kAborting,
                          RolloutState::kRollingBack, RolloutState::kFailed, RolloutState::kRetired},
      /* kAborting    */ {RolloutState::kRollingBack, RolloutState::kFailed, RolloutState::kCompleted,
                          RolloutState::kRetired},
      /* kRollingBack */ {RolloutState::kFailed, RolloutState::kCompleted, RolloutState::kRetired},
      /* kFailed      */ {RolloutState::kRetired},
      /* kCompleted   */ {RolloutState::kRetired},
      /* kRetired     */ {}};
  return table;
}

[[nodiscard]] const std::vector<StageState>& all_stage_states() {
  static const std::vector<StageState> states = {
      StageState::kPending,   StageState::kBlocked,  StageState::kArmed,      StageState::kRunning,
      StageState::kGated,     StageState::kSoaking,  StageState::kAdvancing,  StageState::kSucceeded,
      StageState::kFailed,    StageState::kAborted,  StageState::kRolledBack, StageState::kSkipped};
  return states;
}

[[nodiscard]] const std::vector<std::vector<StageState>>& stage_table() {
  static const std::vector<std::vector<StageState>> table = {
      /* kPending    */ {StageState::kBlocked, StageState::kArmed, StageState::kGated,
                         StageState::kSkipped},
      /* kBlocked    */ {StageState::kArmed, StageState::kAborted, StageState::kSkipped},
      /* kArmed      */ {StageState::kRunning, StageState::kGated, StageState::kAborted,
                         StageState::kRolledBack,
                         StageState::kSkipped},
      /* kRunning    */ {StageState::kGated, StageState::kSoaking, StageState::kAdvancing,
                         StageState::kSucceeded, StageState::kFailed, StageState::kAborted,
                         StageState::kRolledBack},
      /* kGated      */ {StageState::kRunning, StageState::kAdvancing, StageState::kFailed,
                         StageState::kAborted, StageState::kRolledBack},
      /* kSoaking    */ {StageState::kRunning, StageState::kAdvancing, StageState::kFailed,
                         StageState::kAborted, StageState::kRolledBack},
      /* kAdvancing  */ {StageState::kRunning, StageState::kSucceeded, StageState::kFailed,
                         StageState::kAborted, StageState::kRolledBack},
      /* kSucceeded  */ {StageState::kRolledBack},
      /* kFailed     */ {StageState::kRolledBack},
      /* kAborted    */ {StageState::kRolledBack},
      /* kRolledBack */ {},
      /* kSkipped    */ {}};
  return table;
}

[[nodiscard]] const std::vector<TargetState>& all_target_states() {
  static const std::vector<TargetState> states = {
      TargetState::kPending,    TargetState::kDispatched, TargetState::kSucceeded,
      TargetState::kFailed,     TargetState::kCancelled,  TargetState::kRollingBack,
      TargetState::kRolledBack, TargetState::kRollbackFailed, TargetState::kExhausted,
      TargetState::kUnknown};
  return states;
}

[[nodiscard]] const std::vector<std::vector<TargetState>>& target_table() {
  static const std::vector<std::vector<TargetState>> table = {
      /* kPending        */ {TargetState::kDispatched, TargetState::kCancelled,
                             TargetState::kExhausted, TargetState::kUnknown},
      /* kDispatched     */ {TargetState::kPending, TargetState::kSucceeded, TargetState::kFailed,
                             TargetState::kCancelled, TargetState::kUnknown, TargetState::kExhausted},
      /* kSucceeded      */ {TargetState::kRollingBack},
      /* kFailed         */ {TargetState::kRollingBack, TargetState::kExhausted},
      /* kCancelled      */ {TargetState::kRollingBack, TargetState::kExhausted},
      /* kRollingBack    */ {TargetState::kRolledBack, TargetState::kRollbackFailed},
      /* kRolledBack     */ {},
      /* kRollbackFailed */ {},
      /* kExhausted      */ {},
      /* kUnknown        */ {TargetState::kPending, TargetState::kDispatched, TargetState::kSucceeded,
                             TargetState::kFailed, TargetState::kCancelled,
                             TargetState::kExhausted}};
  return table;
}

}  // namespace

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

RF_TEST(digest_sha256_vectors) {
  // FIPS 180-4 / NIST standard vectors.
  RF_CHECK_EQ(sha256_hex(""),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  RF_CHECK_EQ(sha256_hex("abc"),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  RF_CHECK_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  RF_CHECK_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").size(),
              std::size_t{64});

  // The one million 'a' vector: a million block boundary crossings.
  const std::string million_a(1000000u, 'a');
  RF_CHECK_EQ(sha256_hex(million_a),
              std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

  // The digest of the empty message is not the nil digest.
  RF_CHECK(!is_nil(sha256("")));
  RF_CHECK(is_nil(Digest256{}));
}

RF_TEST(digest_sha256_incremental_matches_oneshot) {
  const std::vector<std::size_t> lengths = {0,   1,   2,   3,   55,  56,  57,  63,  64,
                                            65,  119, 120, 127, 128, 129, 255, 256, 1000};
  for (const std::size_t length : lengths) {
    const std::string message = filler(length);
    const std::string expected = sha256_hex(message);

    // One byte at a time through the byte overload.
    Sha256 by_byte;
    for (const char character : message) {
      by_byte.update(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
    RF_CHECK_EQ(to_hex(by_byte.finish()), expected);

    // One byte at a time through the span overload.
    Sha256 by_span;
    for (const char character : message) {
      const std::byte single = static_cast<std::byte>(static_cast<unsigned char>(character));
      by_span.update(std::span<const std::byte>(&single, 1));
    }
    RF_CHECK_EQ(to_hex(by_span.finish()), expected);

    // Arbitrary chunking must not change the result either.
    Sha256 by_chunks;
    std::size_t offset = 0;
    std::size_t chunk = 1;
    while (offset < message.size()) {
      const std::size_t take = (message.size() - offset < chunk) ? (message.size() - offset) : chunk;
      by_chunks.update(std::string_view(message).substr(offset, take));
      offset += take;
      chunk = (chunk * 3u) % 17u + 1u;
    }
    RF_CHECK_EQ(to_hex(by_chunks.finish()), expected);

    // A single span covering the whole message.
    RF_CHECK_EQ(to_hex(sha256(as_bytes(message))), expected);
  }
}

RF_TEST(digest_sha256_peek_and_reset) {
  Sha256 hasher;
  hasher.update("abc");
  const std::string abc = sha256_hex("abc");

  // peek() is non-destructive: it can be called repeatedly and further input
  // still extends the same message.
  RF_CHECK_EQ(to_hex(hasher.peek()), abc);
  RF_CHECK_EQ(to_hex(hasher.peek()), abc);
  hasher.update("def");
  const std::string abcdef = sha256_hex("abcdef");
  RF_CHECK_EQ(to_hex(hasher.peek()), abcdef);

  // finish() returns that same digest and leaves the object reusable.
  RF_CHECK_EQ(to_hex(hasher.finish()), abcdef);
  hasher.update("abc");
  RF_CHECK_EQ(to_hex(hasher.finish()), abc);

  // reset() discards a partially fed message.
  hasher.update("this must not be hashed");
  hasher.reset();
  RF_CHECK_EQ(to_hex(hasher.finish()), sha256_hex(""));

  // A fresh object hashes the empty message.
  Sha256 fresh;
  RF_CHECK_EQ(to_hex(fresh.peek()), sha256_hex(""));
  RF_CHECK_EQ(to_hex(fresh.finish()), sha256_hex(""));

  // Truncation keeps the leading half; widening zero fills the rest.
  const Digest256 wide = sha256("truncate me");
  const Digest128 narrow = truncate128(wide);
  RF_CHECK_EQ(to_hex(narrow), to_hex(wide).substr(0, 32));
  RF_CHECK_EQ(to_hex(widen128(narrow)), to_hex(wide).substr(0, 32) + std::string(32, '0'));
  RF_CHECK_EQ(to_hex(Digest128{}), std::string(32, '0'));
  RF_CHECK_EQ(to_hex(Digest256{}), std::string(64, '0'));
}

// ---------------------------------------------------------------------------
// CRC-32
// ---------------------------------------------------------------------------

RF_TEST(digest_crc32_check_value) {
  // The standard CRC-32 (IEEE 802.3) check value.
  RF_CHECK_EQ(crc32(as_bytes("123456789")), 0xCBF43926u);
  RF_CHECK_EQ(crc32(as_bytes("")), 0x00000000u);
  RF_CHECK_EQ(crc32(as_bytes("a")), 0xE8B7BE43u);
  RF_CHECK_EQ(crc32(as_bytes("abc")), 0x352441C2u);
  RF_CHECK_EQ(crc32(as_bytes("The quick brown fox jumps over the lazy dog")), 0x414FA339u);

  // The seed parameter continues a running CRC: hashing a message in two parts
  // yields the same value as hashing it in one.
  const std::uint32_t part_one = crc32(as_bytes("123"));
  RF_CHECK_EQ(crc32(as_bytes("456789"), part_one), crc32(as_bytes("123456789")));
  RF_CHECK_EQ(crc32(as_bytes("123456789"), 0u), crc32(as_bytes("123456789")));

  // A single flipped bit changes the value.
  RF_CHECK(crc32(as_bytes("123456788")) != crc32(as_bytes("123456789")));
}

// ---------------------------------------------------------------------------
// Hexadecimal encoding and digest comparison
// ---------------------------------------------------------------------------

RF_TEST(digest_hex_roundtrip_and_rejections) {
  const std::vector<std::byte> bytes = {std::byte{0x00}, std::byte{0x01}, std::byte{0x0F},
                                        std::byte{0x10}, std::byte{0x7F}, std::byte{0x80},
                                        std::byte{0xAB}, std::byte{0xFF}};
  RF_CHECK_EQ(to_hex(bytes), std::string("00010f107f80abff"));
  RF_CHECK_EQ(to_hex(std::span<const std::byte>()), std::string(""));

  std::vector<std::byte> parsed(bytes.size(), std::byte{0xEE});
  RF_CHECK(parse_hex("00010f107f80abff", parsed));
  RF_CHECK(parsed == bytes);

  // Uppercase and mixed case input are accepted and produce identical bytes.
  std::vector<std::byte> upper(bytes.size(), std::byte{0x00});
  RF_CHECK(parse_hex("00010F107F80ABFF", upper));
  RF_CHECK(upper == bytes);
  std::vector<std::byte> mixed(bytes.size(), std::byte{0x00});
  RF_CHECK(parse_hex("00010f107F80abFF", mixed));
  RF_CHECK(mixed == bytes);

  // Odd length.
  std::vector<std::byte> target(4, std::byte{0x00});
  RF_CHECK(!parse_hex("00010f1", target));
  // Non-hex characters.
  RF_CHECK(!parse_hex("00010f10zz80abff", parsed));
  RF_CHECK(!parse_hex("00010f10  f80abff", parsed));
  // Wrong length for the destination.
  RF_CHECK(!parse_hex("00010f107f80ab", parsed));
  RF_CHECK(!parse_hex("00010f107f80abff00", parsed));
  RF_CHECK(!parse_hex("", parsed));
  RF_CHECK(!parse_hex("zz", parsed));

  Digest128 digest128{};
  RF_CHECK(parse_hex128("000102030405060708090a0b0c0d0e0f", digest128));
  RF_CHECK_EQ(to_hex(digest128), std::string("000102030405060708090a0b0c0d0e0f"));
  RF_CHECK(!parse_hex128("000102030405060708090a0b0c0d0e", digest128));

  Digest256 digest256{};
  RF_CHECK(parse_hex256("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", digest256));
  RF_CHECK_EQ(to_hex(digest256),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  RF_CHECK(!parse_hex256("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b85", digest256));

  // Round trip through hex for a real digest.
  const Digest256 wide = sha256("hex round trip");
  Digest256 recovered{};
  RF_CHECK(parse_hex256(to_hex(wide), recovered));
  RF_CHECK(recovered == wide);
}

RF_TEST(digest_equal_semantics) {
  const Digest256 left = sha256("left");
  const Digest256 same = sha256("left");
  const Digest256 other = sha256("right");

  RF_CHECK(digest_equal(left, same));
  RF_CHECK(!digest_equal(left, other));
  RF_CHECK(digest_equal(std::span<const std::byte>(), std::span<const std::byte>()));

  // Length mismatch is never equal, even when the prefix matches.
  RF_CHECK(!digest_equal(left, std::span<const std::byte>(left.data(), kDigest256Bytes - 1)));
  RF_CHECK(!digest_equal(truncate128(left), std::span<const std::byte>(left.data(), kDigest256Bytes)));

  // A digest differing in its last byte is not equal.
  Digest256 tweaked = same;
  tweaked[kDigest256Bytes - 1] = static_cast<std::byte>(
      std::to_integer<std::uint8_t>(tweaked[kDigest256Bytes - 1]) ^ 0x01u);
  RF_CHECK(!digest_equal(left, tweaked));
}

// ---------------------------------------------------------------------------
// UTF-8 validation
// ---------------------------------------------------------------------------

RF_TEST(codec_utf8_validation) {
  // Accepted: ASCII and the boundary sequences of every length.
  RF_CHECK(is_valid_utf8(""));
  RF_CHECK(is_valid_utf8("plain ascii"));
  RF_CHECK(is_valid_utf8(utf8_bytes({0x00, 0x7F})));
  RF_CHECK(is_valid_utf8(utf8_bytes({0xC2, 0x80})));              // U+0080, shortest 2 byte form
  RF_CHECK(is_valid_utf8(utf8_bytes({0xDF, 0xBF})));              // U+07FF, longest 2 byte form
  RF_CHECK(is_valid_utf8(utf8_bytes({0xE0, 0xA0, 0x80})));        // U+0800, shortest 3 byte form
  RF_CHECK(is_valid_utf8(utf8_bytes({0xEF, 0xBF, 0xBF})));        // U+FFFF, longest 3 byte form
  RF_CHECK(is_valid_utf8(utf8_bytes({0xF0, 0x90, 0x80, 0x80})));  // U+10000, shortest 4 byte form
  RF_CHECK(is_valid_utf8(utf8_bytes({0xF4, 0x8F, 0xBF, 0xBF})));  // U+10FFFF, maximum code point
  RF_CHECK(is_valid_utf8(utf8_bytes({0xE2, 0x82, 0xAC})));        // U+20AC, the euro sign
  RF_CHECK(is_valid_utf8(utf8_bytes({0xF0, 0x9F, 0x98, 0x80})));  // U+1F600

  // Rejected: overlong forms.
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xC0, 0x80})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xC1, 0xBF})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xE0, 0x80, 0x80})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xE0, 0x9F, 0xBF})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xF0, 0x80, 0x80, 0x80})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xF0, 0x8F, 0xBF, 0xBF})));

  // Rejected: invalid lead bytes, including the five byte forms.
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xF8, 0x88, 0x80, 0x80, 0x80})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xFF})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xFE})));

  // Rejected: truncated sequences.
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xC2})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xE2, 0x82})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xF0, 0x9F, 0x98})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xE2, 0x82, 0xAC, 0xC2})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xF0, 0x9F, 0x98, 0x80, 0xF0})));

  // Rejected: bad continuation bytes.
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xC2, 0x41})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xE2, 0x28, 0xA1})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xF0, 0x9F, 0x98, 0x41})));

  // Rejected: surrogates (U+D800..U+DFFF).
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xED, 0xA0, 0x80})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xED, 0xBF, 0xBF})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xED, 0xAD, 0xBF})));

  // Rejected: beyond U+10FFFF.
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xF4, 0x90, 0x80, 0x80})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xF5, 0x80, 0x80, 0x80})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xF7, 0xBF, 0xBF, 0xBF})));

  // Rejected: lone continuation bytes.
  RF_CHECK(!is_valid_utf8(utf8_bytes({0x80})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0xBF})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0x80, 0x80})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0x41, 0x80, 0x41})));

  // Accepted and rejected sequences embedded in ASCII.
  RF_CHECK(is_valid_utf8(utf8_bytes({0x41, 0xC2, 0xA9, 0x42})));
  RF_CHECK(!is_valid_utf8(utf8_bytes({0x41, 0xC2, 0x42})));
}

// ---------------------------------------------------------------------------
// Bounded binary codec
// ---------------------------------------------------------------------------

RF_TEST(codec_byte_writer_reader_roundtrip) {
  const std::array<std::byte, 3> raw_block = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
  const std::string text = utf8_bytes({0x68, 0x69, 0x20, 0xE2, 0x82, 0xAC});  // "hi <euro sign>"

  ByteWriter writer;
  RF_CHECK(writer.empty());
  RF_CHECK_EQ(writer.size(), std::size_t{0});
  writer.u8(0xABu);
  writer.u16(0x1234u);
  writer.u32(0xDEADBEEFu);
  writer.u64(0x0123456789ABCDEFull);
  writer.i64(-2);
  writer.boolean(true);
  writer.boolean(false);
  writer.presence(true);
  writer.raw(raw_block);
  writer.bytes(raw_block);
  writer.string(text);

  // The canonical little endian layout, byte for byte.
  RF_CHECK_EQ(to_hex(writer.span()),
              std::string("ab3412efbeaddeefcdab8967452301feffffffffffffff0100"
                          "010102030300000001020306000000686920e282ac"));
  RF_CHECK_EQ(writer.size(), std::size_t{1 + 2 + 4 + 8 + 8 + 1 + 1 + 1 + 3 + 4 + 3 + 4 + 6});
  RF_CHECK(!writer.empty());
  RF_CHECK_EQ(writer.data().size(), writer.size());

  ByteReader reader(writer.span());
  RF_CHECK_EQ(reader.remaining(), writer.size());
  RF_CHECK_EQ(reader.position(), std::size_t{0});
  RF_CHECK(!reader.at_end());

  const auto read_u8 = reader.u8();
  RF_REQUIRE(read_u8.ok());
  RF_CHECK_EQ(read_u8.value(), 0xABu);
  const auto read_u16 = reader.u16();
  RF_REQUIRE(read_u16.ok());
  RF_CHECK_EQ(read_u16.value(), 0x1234u);
  const auto read_u32 = reader.u32();
  RF_REQUIRE(read_u32.ok());
  RF_CHECK_EQ(read_u32.value(), 0xDEADBEEFu);
  const auto read_u64 = reader.u64();
  RF_REQUIRE(read_u64.ok());
  RF_CHECK_EQ(read_u64.value(), 0x0123456789ABCDEFull);
  const auto read_i64 = reader.i64();
  RF_REQUIRE(read_i64.ok());
  RF_CHECK_EQ(read_i64.value(), std::int64_t{-2});
  const auto read_true = reader.boolean();
  RF_REQUIRE(read_true.ok());
  RF_CHECK_EQ(read_true.value(), true);
  const auto read_false = reader.boolean();
  RF_REQUIRE(read_false.ok());
  RF_CHECK_EQ(read_false.value(), false);
  const auto read_presence = reader.presence();
  RF_REQUIRE(read_presence.ok());
  RF_CHECK_EQ(read_presence.value(), true);
  const auto read_raw = reader.raw(raw_block.size());
  RF_REQUIRE(read_raw.ok());
  RF_CHECK_EQ(read_raw.value().size(), raw_block.size());
  RF_CHECK(std::equal(read_raw.value().begin(), read_raw.value().end(), raw_block.begin()));
  const auto read_bytes = reader.bytes(16);
  RF_REQUIRE(read_bytes.ok());
  RF_CHECK_EQ(read_bytes.value().size(), raw_block.size());
  RF_CHECK(std::equal(read_bytes.value().begin(), read_bytes.value().end(), raw_block.begin()));
  const auto read_string = reader.string(16);
  RF_REQUIRE(read_string.ok());
  RF_CHECK_EQ(read_string.value(), text);
  RF_CHECK(reader.at_end());
  RF_CHECK_EQ(reader.remaining(), std::size_t{0});
  RF_CHECK(reader.expect_end().ok());

  // owned_bytes() copies out of the reader's input.
  ByteWriter length_prefixed;
  length_prefixed.bytes(raw_block);
  ByteReader owned_reader(length_prefixed.span());
  const auto owned = owned_reader.owned_bytes(16);
  RF_REQUIRE(owned.ok());
  RF_CHECK_EQ(owned.value().size(), raw_block.size());
  RF_CHECK(std::equal(owned.value().begin(), owned.value().end(), raw_block.begin()));
  RF_CHECK(owned_reader.expect_end().ok());

  // clear() and reserve() do not corrupt the encoding.
  writer.clear();
  RF_CHECK(writer.empty());
  writer.reserve(4);
  writer.u32(7u);
  RF_CHECK_EQ(to_hex(writer.span()), std::string("07000000"));
}

RF_TEST(codec_byte_reader_rejections) {
  const std::vector<std::byte> empty;
  const std::span<const std::byte> empty_input(empty);
  ByteReader empty_reader(empty_input);
  RF_CHECK(empty_reader.at_end());
  RF_CHECK(empty_reader.expect_end().ok());
  RF_CHECK(empty_reader.u8().status().code() == StatusCode::kCorrupt);
  RF_CHECK(empty_reader.u16().status().code() == StatusCode::kCorrupt);
  RF_CHECK(empty_reader.u32().status().code() == StatusCode::kCorrupt);
  RF_CHECK(empty_reader.u64().status().code() == StatusCode::kCorrupt);
  RF_CHECK(empty_reader.i64().status().code() == StatusCode::kCorrupt);
  RF_CHECK(empty_reader.boolean().status().code() == StatusCode::kCorrupt);
  RF_CHECK(empty_reader.presence().status().code() == StatusCode::kCorrupt);
  RF_CHECK(empty_reader.raw(1).status().code() == StatusCode::kCorrupt);
  // A failed read must not consume anything.
  RF_CHECK_EQ(empty_reader.position(), std::size_t{0});

  // Truncation: one byte is not a u16, and raw() cannot over-read.
  const std::vector<std::byte> one = {std::byte{0x2A}};
  const std::span<const std::byte> one_input(one);
  ByteReader one_reader(one_input);
  RF_CHECK(one_reader.u16().status().code() == StatusCode::kCorrupt);
  RF_CHECK_EQ(one_reader.position(), std::size_t{0});
  const auto one_u8 = one_reader.u8();
  RF_REQUIRE(one_u8.ok());
  RF_CHECK_EQ(one_u8.value(), 0x2Au);
  RF_CHECK(one_reader.raw(1).status().code() == StatusCode::kCorrupt);

  // A declared length above the caller's maximum is kLimitExceeded, and it is
  // reported before any payload is read.
  ByteWriter oversized;
  oversized.u32(1000u);
  ByteReader oversized_reader(oversized.span());
  const auto too_long = oversized_reader.bytes(16);
  RF_REQUIRE(!too_long.ok());
  RF_CHECK(too_long.status().code() == StatusCode::kLimitExceeded);
  RF_CHECK(too_long.status().message().find("exceeds") != std::string::npos);
  RF_CHECK(too_long.status().message().find("1000") != std::string::npos);

  // A declared length within the maximum but with a missing payload is
  // kCorrupt, not kLimitExceeded.
  ByteWriter truncated;
  truncated.u32(8u);
  truncated.u8(0x01u);
  ByteReader truncated_reader(truncated.span());
  RF_CHECK(truncated_reader.bytes(16).status().code() == StatusCode::kCorrupt);
  // The same holds for a length prefix that is itself truncated.
  ByteWriter split_prefix;
  split_prefix.u8(0x01u);
  split_prefix.u8(0x00u);
  ByteReader split_reader(split_prefix.span());
  RF_CHECK(split_reader.bytes(16).status().code() == StatusCode::kCorrupt);

  // Invalid booleans: only 0 and 1 are accepted.
  const std::vector<std::byte> bad_bool = {std::byte{0x02}};
  const std::span<const std::byte> bad_bool_input(bad_bool);
  ByteReader bad_bool_reader(bad_bool_input);
  RF_CHECK(bad_bool_reader.boolean().status().code() == StatusCode::kCorrupt);
  const std::vector<std::byte> also_bad_bool = {std::byte{0xFF}};
  const std::span<const std::byte> also_bad_input(also_bad_bool);
  ByteReader also_bad_reader(also_bad_input);
  RF_CHECK(also_bad_reader.presence().status().code() == StatusCode::kCorrupt);

  // Non UTF-8 strings: invalid lead bytes, overlong forms and lone
  // continuations are all rejected, including when the length prefix is valid.
  ByteWriter bad_text;
  bad_text.u32(3u);
  bad_text.u8(0xFFu);
  bad_text.u8(0xFFu);
  bad_text.u8(0xFFu);
  ByteReader bad_text_reader(bad_text.span());
  const auto bad_string = bad_text_reader.string(16);
  RF_REQUIRE(!bad_string.ok());
  RF_CHECK(bad_string.status().code() == StatusCode::kCorrupt);
  RF_CHECK(bad_string.status().message().find("UTF-8") != std::string::npos);

  ByteWriter overlong_text;
  overlong_text.string(utf8_bytes({0xC0, 0x80}));
  ByteReader overlong_reader(overlong_text.span());
  RF_CHECK(overlong_reader.string(16).status().code() == StatusCode::kCorrupt);

  ByteWriter lone_continuation;
  lone_continuation.string(utf8_bytes({0x41, 0x80}));
  ByteReader continuation_reader(lone_continuation.span());
  RF_CHECK(continuation_reader.string(16).status().code() == StatusCode::kCorrupt);

  ByteWriter surrogate_text;
  surrogate_text.string(utf8_bytes({0xED, 0xA0, 0x80}));
  ByteReader surrogate_reader(surrogate_text.span());
  RF_CHECK(surrogate_reader.string(16).status().code() == StatusCode::kCorrupt);

  // Trailing bytes are rejected by expect_end().
  const std::vector<std::byte> trailing = {std::byte{0x01}, std::byte{0x02}};
  const std::span<const std::byte> trailing_input(trailing);
  ByteReader trailing_reader(trailing_input);
  const auto first = trailing_reader.u8();
  RF_REQUIRE(first.ok());
  RF_CHECK(!trailing_reader.at_end());
  RF_CHECK_EQ(trailing_reader.remaining(), std::size_t{1});
  const Status trailing_status = trailing_reader.expect_end();
  RF_CHECK(!trailing_status.ok());
  RF_CHECK(trailing_status.code() == StatusCode::kCorrupt);
  RF_CHECK(trailing_status.message().find("trailing") != std::string::npos);
  const auto second = trailing_reader.u8();
  RF_REQUIRE(second.ok());
  RF_CHECK(trailing_reader.expect_end().ok());
}

// ---------------------------------------------------------------------------
// Checked size arithmetic
// ---------------------------------------------------------------------------

RF_TEST(codec_checked_arithmetic) {
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  constexpr std::uint64_t kTwo32 = 1ull << 32;

  std::uint64_t out = 0xDEADBEEFull;

  // Addition.
  RF_CHECK(checked_add(0u, 0u, out));
  RF_CHECK_EQ(out, 0ull);
  RF_CHECK(checked_add(kMax - 1u, 1u, out));
  RF_CHECK_EQ(out, kMax);
  RF_CHECK(checked_add(kMax, 0u, out));
  RF_CHECK_EQ(out, kMax);
  RF_CHECK(checked_add(kMax / 2u, kMax / 2u, out));
  RF_CHECK_EQ(out, kMax - 1u);
  out = 0xDEADBEEFull;
  RF_CHECK(!checked_add(kMax, 1u, out));
  RF_CHECK_EQ(out, 0xDEADBEEFull);  // untouched on failure
  RF_CHECK(!checked_add(kMax / 2u + 1u, kMax / 2u + 1u, out));
  RF_CHECK_EQ(out, 0xDEADBEEFull);
  RF_CHECK(!checked_add(kMax, kMax, out));
  RF_CHECK_EQ(out, 0xDEADBEEFull);

  // Multiplication.
  RF_CHECK(checked_mul(0u, kMax, out));
  RF_CHECK_EQ(out, 0ull);
  RF_CHECK(checked_mul(kMax, 0u, out));
  RF_CHECK_EQ(out, 0ull);
  RF_CHECK(checked_mul(kMax, 1u, out));
  RF_CHECK_EQ(out, kMax);
  RF_CHECK(checked_mul(1u, kMax, out));
  RF_CHECK_EQ(out, kMax);
  RF_CHECK(checked_mul(kTwo32, kTwo32 - 1u, out));
  RF_CHECK_EQ(out, 0xFFFFFFFF00000000ull);
  RF_CHECK(checked_mul(kTwo32 - 1u, kTwo32 + 1u, out));
  RF_CHECK_EQ(out, kMax);
  RF_CHECK(checked_mul(3u, 5u, out));
  RF_CHECK_EQ(out, 15ull);
  out = 0xDEADBEEFull;
  RF_CHECK(!checked_mul(kTwo32, kTwo32, out));
  RF_CHECK_EQ(out, 0xDEADBEEFull);
  RF_CHECK(!checked_mul(kMax, kMax, out));
  RF_CHECK_EQ(out, 0xDEADBEEFull);
  RF_CHECK(!checked_mul(2u, kMax / 2u + 1u, out));
  RF_CHECK(!checked_mul(kMax, 2u, out));
  RF_CHECK(!checked_mul(kMax, kMax - 1u, out));
  RF_CHECK_EQ(out, 0xDEADBEEFull);
}

RF_TEST(codec_compare_products_reference) {
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  constexpr std::uint64_t kTwo32 = 1ull << 32;

  // Reference computed with plain 64-bit arithmetic: every product below stays
  // far inside the exact range.
  for (std::uint64_t lhs_a = 0; lhs_a <= 8; ++lhs_a) {
    for (std::uint64_t lhs_b = 0; lhs_b <= 8; ++lhs_b) {
      for (std::uint64_t rhs_a = 0; rhs_a <= 8; ++rhs_a) {
        for (std::uint64_t rhs_b = 0; rhs_b <= 8; ++rhs_b) {
          const std::uint64_t left = lhs_a * lhs_b;
          const std::uint64_t right = rhs_a * rhs_b;
          const int expected = (left < right) ? -1 : ((left > right) ? 1 : 0);
          const int actual = compare_products(lhs_a, lhs_b, rhs_a, rhs_b);
          if (actual != expected) {
            RF_FAIL("compare_products(" + std::to_string(lhs_a) + ", " + std::to_string(lhs_b) +
                    ", " + std::to_string(rhs_a) + ", " + std::to_string(rhs_b) + ") was " +
                    std::to_string(actual) + ", expected " + std::to_string(expected));
          }
        }
      }
    }
  }

  // Values whose products only fit in 128 bits: the expectations are the exact
  // mathematical results.
  RF_CHECK_EQ(compare_products(kMax, kMax, kMax, kMax), 0);
  RF_CHECK_EQ(compare_products(kMax, kMax, 1u, 1u), 1);
  RF_CHECK_EQ(compare_products(1u, 1u, kMax, kMax), -1);
  RF_CHECK_EQ(compare_products(kMax, 1u, 1u, kMax), 0);
  RF_CHECK_EQ(compare_products(0u, kMax, 0u, 1u), 0);
  RF_CHECK_EQ(compare_products(0u, kMax, 1u, 1u), -1);
  RF_CHECK_EQ(compare_products(kTwo32, kTwo32, kMax, 1u), 1);            // 2^64 > 2^64 - 1
  RF_CHECK_EQ(compare_products(kTwo32, kTwo32 - 1u, kMax, 1u), -1);      // 2^64 - 2^32 < 2^64 - 1
  RF_CHECK_EQ(compare_products(kTwo32 - 1u, kTwo32 + 1u, kMax, 1u), 0);  // both are 2^64 - 1
  RF_CHECK_EQ(compare_products(kTwo32 + 1u, kTwo32 + 1u, kTwo32, kTwo32), 1);
  RF_CHECK_EQ(compare_products(kTwo32, kTwo32, kTwo32 + 1u, kTwo32 - 1u), 1);
  RF_CHECK_EQ(compare_products(kMax - 1u, kMax - 1u, kMax, kMax - 2u), 1);
  RF_CHECK_EQ(compare_products(kMax, kMax - 1u, kMax - 1u, kMax), 0);
}

// ---------------------------------------------------------------------------
// Strong identifiers
// ---------------------------------------------------------------------------

RF_TEST(ids_strong_id_roundtrip_and_ordering) {
  const TargetId nil;
  RF_CHECK(nil.is_nil());
  RF_CHECK_EQ(nil.to_hex(), std::string(32, '0'));
  RF_CHECK_EQ(nil.bytes().size(), kDigest128Bytes);
  RF_CHECK_EQ(nil.span().size(), kDigest128Bytes);

  std::array<std::byte, kDigest128Bytes> pattern{};
  for (std::size_t position = 0; position < pattern.size(); ++position) {
    pattern[position] = static_cast<std::byte>(static_cast<std::uint8_t>(position));
  }
  const TargetId id = TargetId::from_bytes(std::span<const std::byte, kDigest128Bytes>(pattern));
  RF_CHECK(!id.is_nil());
  RF_CHECK_EQ(id.to_hex(), std::string("000102030405060708090a0b0c0d0e0f"));
  RF_CHECK(id.bytes() == pattern);

  // parse() accepts both hex cases and rejects malformed input.
  const std::optional<TargetId> parsed = TargetId::parse(id.to_hex());
  RF_REQUIRE(parsed.has_value());
  RF_CHECK(*parsed == id);
  RF_CHECK_EQ(parsed->to_hex(), id.to_hex());
  const std::optional<TargetId> upper = TargetId::parse("000102030405060708090A0B0C0D0E0F");
  RF_REQUIRE(upper.has_value());
  RF_CHECK(*upper == id);
  RF_CHECK(!TargetId::parse("").has_value());
  RF_CHECK(!TargetId::parse("000102030405060708090a0b0c0d0e").has_value());
  RF_CHECK(!TargetId::parse("000102030405060708090a0b0c0d0e0f00").has_value());
  RF_CHECK(!TargetId::parse("000102030405060708090a0b0c0d0e0g").has_value());
  RF_CHECK(!TargetId::parse("000102030405060708090a0b0c0d0e f").has_value());
  const std::optional<TargetId> zeros = TargetId::parse(std::string(32, '0'));
  RF_REQUIRE(zeros.has_value());
  RF_CHECK(zeros->is_nil());

  // Ordering is the lexicographic order of the raw bytes.
  std::array<std::byte, kDigest128Bytes> high_bytes{};
  high_bytes[0] = std::byte{0x01};
  std::array<std::byte, kDigest128Bytes> later_bytes{};
  later_bytes[kDigest128Bytes - 1] = std::byte{0x01};
  const TargetId low = TargetId{};
  const TargetId high = TargetId::from_bytes(std::span<const std::byte, kDigest128Bytes>(high_bytes));
  const TargetId later =
      TargetId::from_bytes(std::span<const std::byte, kDigest128Bytes>(later_bytes));
  RF_CHECK(low < high);
  RF_CHECK(high > low);
  RF_CHECK(low <= low);
  RF_CHECK(low == low);
  RF_CHECK(low != high);
  RF_CHECK(low < later);
  RF_CHECK(later < high);

  // Distinct tag types are distinct C++ types: a TargetId can never be compared
  // with, converted to or constructed from an AttemptId, so the compiler - not a
  // runtime check - is what keeps the identity domains apart. That is a
  // compile-time property, so it is asserted here as a type-level fact rather
  // than as an executable check.
  static_assert(!std::is_same_v<TargetId, AttemptId>, "identity tags must not collapse");
  static_assert(!std::is_convertible_v<TargetId, AttemptId>, "identities must not convert");
  static_assert(!std::is_constructible_v<AttemptId, TargetId>, "identities must not construct");
  static_assert(!std::is_convertible_v<AttemptId, RolloutId>, "identities must not convert");
  static_assert(!std::is_same_v<StrongId<TargetIdTag>, StrongId<AttemptIdTag>>, "tags separate");

  // The hash functor agrees with equality.
  const StrongIdHash<TargetIdTag> hasher;
  RF_CHECK_EQ(hasher(id), hasher(*parsed));
  RF_CHECK(hasher(id) != hasher(high));
}

RF_TEST(ids_factory_determinism_and_resume) {
  const Digest256 seed_a = sha256("seed-a");
  const Digest256 seed_b = sha256("seed-b");

  IdFactory first(seed_a);
  RF_CHECK(first.seed() == seed_a);
  RF_CHECK_EQ(first.counter(), 0ull);
  const RolloutId first_rollout = first.next<RolloutId>(IdDomain::kRollout);
  const RolloutId second_rollout = first.next<RolloutId>(IdDomain::kRollout);
  const AttemptId first_attempt = first.next<AttemptId>(IdDomain::kAttempt);
  const StageId first_stage = first.next<StageId>(IdDomain::kStage);
  RF_CHECK_EQ(first.counter(), 4ull);
  RF_CHECK(!first_rollout.is_nil());
  RF_CHECK(first_rollout != second_rollout);

  // The same seed and the same call sequence produce identical identifiers.
  IdFactory second(seed_a);
  RF_CHECK(second.next<RolloutId>(IdDomain::kRollout) == first_rollout);
  RF_CHECK(second.next<RolloutId>(IdDomain::kRollout) == second_rollout);
  RF_CHECK(second.next<AttemptId>(IdDomain::kAttempt) == first_attempt);
  RF_CHECK(second.next<StageId>(IdDomain::kStage) == first_stage);
  RF_CHECK_EQ(second.counter(), 4ull);

  // A different seed produces a different identifier for the same call.
  IdFactory other(seed_b);
  const RolloutId other_rollout = other.next<RolloutId>(IdDomain::kRollout);
  RF_CHECK(other_rollout != first_rollout);
  RF_CHECK_EQ(other.counter(), 1ull);

  // set_counter() resumes the sequence: replaying from counter 2 reproduces
  // exactly the identifiers the original run minted at those positions.
  IdFactory resumed(seed_a);
  resumed.set_counter(2u);
  RF_CHECK_EQ(resumed.counter(), 2ull);
  RF_CHECK(resumed.next<AttemptId>(IdDomain::kAttempt) == first_attempt);
  RF_CHECK(resumed.next<StageId>(IdDomain::kStage) == first_stage);
  RF_CHECK_EQ(resumed.counter(), 4ull);

  // Resuming a different seed at the same counter does not collide.
  IdFactory other_resumed(seed_b);
  other_resumed.set_counter(2u);
  RF_CHECK(other_resumed.next<AttemptId>(IdDomain::kAttempt) != first_attempt);

  // The domain tag is part of the derivation: the same seed, the same counter
  // and different domains produce different identifiers.
  IdFactory domain_a(seed_a);
  IdFactory domain_b(seed_a);
  const StageId stage_at_zero = domain_a.next<StageId>(IdDomain::kStage);
  const CohortId cohort_at_zero = domain_b.next<CohortId>(IdDomain::kCohort);
  RF_CHECK(stage_at_zero.to_hex() != cohort_at_zero.to_hex());

  // Distinguishable domain names are part of the persisted identity space.
  RF_CHECK_EQ(std::string(to_string(IdDomain::kRollout)), std::string("rollout"));
  RF_CHECK_EQ(std::string(to_string(IdDomain::kFailureDomain)), std::string("failure_domain"));
}

// ---------------------------------------------------------------------------
// Timestamps
// ---------------------------------------------------------------------------

RF_TEST(time_timestamp_rfc3339_roundtrip) {
  // The epoch.
  RF_CHECK_EQ(Timestamp::from_unix_seconds(0).to_rfc3339(), std::string("1970-01-01T00:00:00.000Z"));
  RF_CHECK_EQ(Timestamp::from_unix_seconds(0).unix_nanos(), std::int64_t{0});
  const auto epoch = Timestamp::parse_rfc3339("1970-01-01T00:00:00Z");
  RF_REQUIRE(epoch.ok());
  RF_CHECK_EQ(epoch.value().unix_nanos(), std::int64_t{0});
  RF_CHECK_EQ(epoch.value().to_rfc3339(), std::string("1970-01-01T00:00:00.000Z"));
  // Note: the runtime treats a zero nanosecond count as nil, so the epoch
  // doubles as "no observation recorded" and evaluate_freshness() rejects it.
  RF_CHECK(epoch.value().is_nil());

  // A leap day. 2024-02-29T00:00:00Z is 1,709,164,800 seconds after the epoch.
  const auto leap = Timestamp::parse_rfc3339("2024-02-29T00:00:00Z");
  RF_REQUIRE(leap.ok());
  RF_CHECK_EQ(leap.value().unix_millis(), std::int64_t{1709164800000});
  RF_CHECK_EQ(leap.value().to_rfc3339(), std::string("2024-02-29T00:00:00.000Z"));
  RF_CHECK_EQ(Timestamp::from_unix_seconds(1709164800).to_rfc3339(),
              std::string("2024-02-29T00:00:00.000Z"));

  // Fractional seconds are rendered with millisecond precision.
  const auto fractional = Timestamp::parse_rfc3339("2024-02-29T12:34:56.789Z");
  RF_REQUIRE(fractional.ok());
  RF_CHECK_EQ(fractional.value().unix_millis(), std::int64_t{1709210096789});
  RF_CHECK_EQ(fractional.value().to_rfc3339(), std::string("2024-02-29T12:34:56.789Z"));
  const auto short_fraction = Timestamp::parse_rfc3339("2024-02-29T12:34:56.5Z");
  RF_REQUIRE(short_fraction.ok());
  RF_CHECK_EQ(short_fraction.value().unix_millis(), std::int64_t{1709210096500});
  RF_CHECK_EQ(short_fraction.value().to_rfc3339(), std::string("2024-02-29T12:34:56.500Z"));

  // Pre-1970 instants with whole seconds round trip through the civil calendar.
  const auto before_epoch = Timestamp::parse_rfc3339("1969-12-31T23:59:59Z");
  RF_REQUIRE(before_epoch.ok());
  RF_CHECK_EQ(before_epoch.value().unix_nanos(), std::int64_t{-1000000000});
  RF_CHECK_EQ(before_epoch.value().to_rfc3339(), std::string("1969-12-31T23:59:59.000Z"));
  RF_CHECK_EQ(Timestamp::from_unix_seconds(-86400).to_rfc3339(),
              std::string("1969-12-31T00:00:00.000Z"));
  RF_CHECK_EQ(Timestamp::from_unix_seconds(-2208988800).to_rfc3339(),
              std::string("1900-01-01T00:00:00.000Z"));

  // Numeric offsets are normalised to UTC.
  const auto plus_two = Timestamp::parse_rfc3339("2024-02-29T12:34:56+02:00");
  RF_REQUIRE(plus_two.ok());
  RF_CHECK_EQ(plus_two.value().unix_millis(), std::int64_t{1709202896000});
  RF_CHECK_EQ(plus_two.value().to_rfc3339(), std::string("2024-02-29T10:34:56.000Z"));
  const auto minus_five_thirty = Timestamp::parse_rfc3339("2024-02-29T12:34:56-05:30");
  RF_REQUIRE(minus_five_thirty.ok());
  RF_CHECK_EQ(minus_five_thirty.value().to_rfc3339(), std::string("2024-02-29T18:04:56.000Z"));
  const auto utc_reference = Timestamp::parse_rfc3339("2024-02-29T10:34:56Z");
  RF_REQUIRE(utc_reference.ok());
  RF_CHECK(plus_two.value() == utc_reference.value());

  // Lowercase designators are accepted.
  const auto lowercase = Timestamp::parse_rfc3339("2024-02-29t12:34:56z");
  RF_REQUIRE(lowercase.ok());
  RF_CHECK(lowercase.value() == fractional.value() - Duration::from_millis(789));

  // Arithmetic on timestamps and durations.
  const Timestamp base = Timestamp::from_unix_millis(1700000000000);
  RF_CHECK_EQ((base + Duration::from_millis(1500)).unix_millis(), std::int64_t{1700000001500});
  RF_CHECK_EQ((base - Duration::from_millis(1500)).unix_millis(), std::int64_t{1699999998500});
  RF_CHECK((base + Duration::from_seconds(60)) - base == Duration::from_seconds(60));
  RF_CHECK(Duration::from_seconds(2) > Duration::from_millis(1999));
  RF_CHECK(Duration::from_millis(-5).is_negative());
  RF_CHECK(Duration{}.is_zero());

  // Uninitialised timestamps are nil and render as the epoch.
  RF_CHECK(Timestamp{}.is_nil());
  RF_CHECK_EQ(Timestamp{}.to_rfc3339(), std::string("1970-01-01T00:00:00.000Z"));
}

// Regression test: a pre-1970 instant whose sub-second part is non-zero must
// render as a valid RFC 3339 timestamp and round trip through parse_rfc3339().
// Splitting a negative nanosecond count needs floor division; truncation
// towards zero places 1969-12-31T23:59:59.750Z one second into 1970 and prints
// a negative fractional part:
//   Timestamp::from_unix_nanos(-1500000000).to_rfc3339()
//     wrong: "1969-12-31T23:59:59.-500Z"   right: "1969-12-31T23:59:58.500Z"
//   Timestamp::from_unix_nanos(-250000000).to_rfc3339()
//     wrong: "1970-01-01T00:00:00.-250Z"   right: "1969-12-31T23:59:59.750Z"
// The wrong form is not parseable, so the instant could not survive a round
// trip. Whole-second pre-epoch instants are covered by
// time_timestamp_rfc3339_roundtrip above.
RF_TEST(time_rfc3339_pre_epoch_fractional_roundtrip) {
  const Timestamp half_before = Timestamp::from_unix_nanos(-1500000000);  // 1969-12-31T23:59:58.500Z
  RF_CHECK_EQ(half_before.to_rfc3339(), std::string("1969-12-31T23:59:58.500Z"));

  const Timestamp quarter_before = Timestamp::from_unix_nanos(-250000000);  // 1969-12-31T23:59:59.750Z
  RF_CHECK_EQ(quarter_before.to_rfc3339(), std::string("1969-12-31T23:59:59.750Z"));

  const auto reparsed = Timestamp::parse_rfc3339(half_before.to_rfc3339());
  RF_REQUIRE(reparsed.ok());
  RF_CHECK(reparsed.value() == half_before);
}

RF_TEST(time_parse_rfc3339_rejections) {
  const std::vector<std::string> rejected = {
      "",
      "1970",
      "1970-01-01",
      "1970-01-01T00:00:00",        // no zone designator
      "1970-01-01T00:00:00Z ",      // trailing space
      " 1970-01-01T00:00:00Z",      // leading space
      "1970-01-01 00:00:00Z",       // space instead of T
      "1970-01-01T00:00:00X",       // unknown zone designator
      "1970-01-01T00:00:00ZZ",      // trailing characters
      "1970-01-01T00:00:00Zjunk",   // trailing characters
      "1970-01-01T00:00:00.Z",      // empty fractional part
      "1970-01-01T00:00:00,5Z",     // comma instead of a full stop
      "1970-13-01T00:00:00Z",       // month out of range
      "1970-00-01T00:00:00Z",       // month zero
      "1970-01-00T00:00:00Z",       // day zero
      "1970-01-32T00:00:00Z",       // day out of range
      "1970-01-01T24:00:00Z",       // hour out of range
      "1970-01-01T00:60:00Z",       // minute out of range
      "1970-01-01T00:00:61Z",       // second out of range
      "1970-01-01T00:00:00+2:00",   // malformed offset
      "1970-01-01T00:00:00+02:0",   // malformed offset
      "1970-01-01T00:00:00+99:00",  // offset out of range
      "1970-01-01T00:00:00+00:99",  // offset out of range
      "1970-01-01T00:00:00-",       // truncated offset
      "19700101T000000Z",           // missing separators
      "1970-01-01T00-00-00Z",       // wrong separators
      "abcdefghijklmnopqrst",       // not digits at all
  };
  for (const std::string& text : rejected) {
    const auto parsed = Timestamp::parse_rfc3339(text);
    if (parsed.ok()) {
      RF_FAIL("parse_rfc3339(\"" + text + "\") was accepted as " +
              std::to_string(parsed.value().unix_nanos()) + " ns");
    }
    RF_CHECK(parsed.status().code() == StatusCode::kInvalidArgument);
  }

  // Accepted forms, including the three leniencies the parser has today: an
  // offset may omit the colon, a day number that does not exist in the month is
  // normalised forward, and a leap second is accepted. They are recorded here
  // as observed behaviour so that a future tightening shows up as a failure
  // rather than as a silent change. RFC 3339 requires the colon in the offset
  // and does not allow 2023-02-29.
  RF_CHECK(Timestamp::parse_rfc3339("1970-01-01T00:00:00+0200").ok());
  const auto normalised = Timestamp::parse_rfc3339("2023-02-29T00:00:00Z");
  RF_REQUIRE(normalised.ok());
  RF_CHECK_EQ(normalised.value().to_rfc3339(), std::string("2023-03-01T00:00:00.000Z"));
  const auto leap_second = Timestamp::parse_rfc3339("1970-01-01T00:00:60Z");
  RF_REQUIRE(leap_second.ok());
  RF_CHECK_EQ(leap_second.value().unix_nanos(), std::int64_t{60000000000});
}

// ---------------------------------------------------------------------------
// Freshness
// ---------------------------------------------------------------------------

RF_TEST(time_evaluate_freshness) {
  const Timestamp now = Timestamp::from_unix_millis(1700000000000);
  const Duration max_age = Duration::from_seconds(30);
  const Duration skew = Duration::from_seconds(5);

  // Fresh.
  const Freshness fresh = evaluate_freshness(now - Duration::from_millis(100), now, max_age, skew);
  RF_CHECK(fresh.fresh);
  RF_CHECK(static_cast<bool>(fresh));
  RF_CHECK(fresh.age == Duration::from_millis(100));
  RF_CHECK(fresh.reason.find("100 ms") != std::string::npos);

  // Exactly at the age ceiling is still fresh; one nanosecond past it is not.
  RF_CHECK(evaluate_freshness(now - max_age, now, max_age, skew).fresh);
  const Freshness just_stale =
      evaluate_freshness(now - max_age - Duration::from_nanos(1), now, max_age, skew);
  RF_CHECK(!just_stale.fresh);
  RF_CHECK(!static_cast<bool>(just_stale));

  // Stale.
  const Freshness stale = evaluate_freshness(now - Duration::from_seconds(31), now, max_age, skew);
  RF_CHECK(!stale.fresh);
  RF_CHECK(stale.age == Duration::from_seconds(31));
  RF_CHECK(stale.reason.find("older than") != std::string::npos);

  // Zero age is fresh.
  RF_CHECK(evaluate_freshness(now, now, max_age, skew).fresh);

  // Within the permitted clock skew.
  const Freshness within_skew = evaluate_freshness(now + Duration::from_millis(500), now, max_age, skew);
  RF_CHECK(within_skew.fresh);
  RF_CHECK(within_skew.age.is_negative());
  RF_CHECK(within_skew.reason.find("skew") != std::string::npos);
  RF_CHECK(evaluate_freshness(now + skew, now, max_age, skew).fresh);

  // Beyond the permitted clock skew.
  const Freshness beyond_skew =
      evaluate_freshness(now + skew + Duration::from_nanos(1), now, max_age, skew);
  RF_CHECK(!beyond_skew.fresh);
  RF_CHECK(beyond_skew.reason.find("future") != std::string::npos);
  const Freshness far_future = evaluate_freshness(now + Duration::from_seconds(60), now, max_age, skew);
  RF_CHECK(!far_future.fresh);

  // A nil observation time is never fresh, whatever the other arguments say.
  const Freshness missing = evaluate_freshness(Timestamp{}, now, max_age, skew);
  RF_CHECK(!missing.fresh);
  RF_CHECK(missing.age.is_zero());
  RF_CHECK(missing.reason.find("no observation") != std::string::npos);

  // A zero max_age accepts only an exactly current sample.
  RF_CHECK(evaluate_freshness(now, now, Duration{}, skew).fresh);
  RF_CHECK(!evaluate_freshness(now - Duration::from_nanos(1), now, Duration{}, skew).fresh);
}

// ---------------------------------------------------------------------------
// Ratios
// ---------------------------------------------------------------------------

RF_TEST(policy_ratio_validation) {
  RF_CHECK(ratio(0u, 1u).validate().ok());
  RF_CHECK(ratio(1u, 1u).validate().ok());
  RF_CHECK(ratio(3u, 4u).validate().ok());
  RF_CHECK(ratio(0u, 1000000u).validate().ok());

  const Status zero_denominator = ratio(1u, 0u).validate();
  RF_CHECK(!zero_denominator.ok());
  RF_CHECK(zero_denominator.code() == StatusCode::kInvalidArgument);
  RF_CHECK(zero_denominator.message().find("non-zero denominator") != std::string::npos);

  const Status inverted = ratio(5u, 4u).validate();
  RF_CHECK(!inverted.ok());
  RF_CHECK(inverted.code() == StatusCode::kInvalidArgument);
  RF_CHECK(!ratio(0u, 0u).validate().ok());

  RF_CHECK(ratio(0u, 4u).is_zero());
  RF_CHECK(!ratio(1u, 4u).is_zero());
  RF_CHECK(ratio(7u, 7u).is_one());
  RF_CHECK(!ratio(6u, 7u).is_one());

  // to_string() renders the stored fields, validated or not.
  RF_CHECK_EQ(ratio(3u, 4u).to_string(), std::string("3/4"));
  RF_CHECK_EQ(ratio(0u, 1u).to_string(), std::string("0/1"));
  RF_CHECK_EQ(ratio(1u, 0u).to_string(), std::string("1/0"));
  RF_CHECK_EQ(ratio(4294967295u, 4294967295u).to_string(), std::string("4294967295/4294967295"));

  // compare() orders the exact values, not the field pairs.
  RF_CHECK_EQ(ratio(1u, 2u).compare(ratio(2u, 4u)), 0);
  RF_CHECK_EQ(ratio(1u, 3u).compare(ratio(1u, 2u)), -1);
  RF_CHECK_EQ(ratio(2u, 3u).compare(ratio(1u, 2u)), 1);
  RF_CHECK_EQ(ratio(0u, 1u).compare(ratio(1u, 1000000u)), -1);
  RF_CHECK_EQ(ratio(1u, 1u).compare(ratio(1u, 1u)), 0);
  RF_CHECK(ratio(1u, 2u) != ratio(1u, 3u));
  RF_CHECK(ratio(1u, 2u) == ratio(1u, 2u));
}

RF_TEST(policy_ratio_apply_and_satisfied_by) {
  // The reference values below are computed with plain 64-bit arithmetic on
  // inputs whose products stay far inside the exact range.
  const std::vector<std::uint64_t> values = {0,   1,    2,    3,      7,      10,     99,
                                             100, 1000, 12345, 999999, 1000000};
  const std::vector<Ratio> ratios = {ratio(0u, 1u),   ratio(1u, 1u),  ratio(1u, 2u),
                                     ratio(1u, 3u),   ratio(2u, 3u),  ratio(3u, 4u),
                                     ratio(7u, 9u),   ratio(999u, 1000u), ratio(1u, 1000000u)};
  for (const Ratio& value_ratio : ratios) {
    for (const std::uint64_t value : values) {
      const std::uint64_t numerator = value_ratio.numerator;
      const std::uint64_t denominator = value_ratio.denominator;
      const std::uint64_t expected_floor = (value * numerator) / denominator;
      const std::uint64_t expected_ceil = (value * numerator + denominator - 1u) / denominator;
      if (value_ratio.apply_floor(value) != expected_floor) {
        RF_FAIL("apply_floor(" + std::to_string(value) + ") for " + value_ratio.to_string() +
                " was " + std::to_string(value_ratio.apply_floor(value)) + ", expected " +
                std::to_string(expected_floor));
      }
      if (value_ratio.apply_ceil(value) != expected_ceil) {
        RF_FAIL("apply_ceil(" + std::to_string(value) + ") for " + value_ratio.to_string() +
                " was " + std::to_string(value_ratio.apply_ceil(value)) + ", expected " +
                std::to_string(expected_ceil));
      }
      // Floor and ceiling bracket the exact value and never exceed the input.
      RF_CHECK(value_ratio.apply_floor(value) <= value);
      RF_CHECK(value_ratio.apply_ceil(value) <= value);
      RF_CHECK(value_ratio.apply_floor(value) <= value_ratio.apply_ceil(value));
      RF_CHECK(value_ratio.apply_ceil(value) - value_ratio.apply_floor(value) <= 1u);
    }
  }

  // Exact behaviour at the edges: a zero ratio and a zero value both yield zero,
  // and a whole ratio is the identity.
  RF_CHECK_EQ(ratio(0u, 1u).apply_floor(1000000u), 0ull);
  RF_CHECK_EQ(ratio(0u, 1u).apply_ceil(1000000u), 0ull);
  RF_CHECK_EQ(ratio(1u, 1u).apply_floor(1000000u), 1000000ull);
  RF_CHECK_EQ(ratio(1u, 1u).apply_ceil(1000000u), 1000000ull);
  RF_CHECK_EQ(ratio(3u, 4u).apply_floor(0u), 0ull);
  RF_CHECK_EQ(ratio(3u, 4u).apply_ceil(0u), 0ull);
  // 3/4 of 1000 is exactly 750; 2/3 of 7 is 4.666..., so floor 4 and ceil 5.
  RF_CHECK_EQ(ratio(3u, 4u).apply_floor(1000u), 750ull);
  RF_CHECK_EQ(ratio(3u, 4u).apply_ceil(1000u), 750ull);
  RF_CHECK_EQ(ratio(2u, 3u).apply_floor(7u), 4ull);
  RF_CHECK_EQ(ratio(2u, 3u).apply_ceil(7u), 5ull);
  RF_CHECK_EQ(ratio(1u, 3u).apply_floor(10u), 3ull);
  RF_CHECK_EQ(ratio(1u, 3u).apply_ceil(10u), 4ull);
  // A zero denominator cannot be validated, and the arithmetic refuses to
  // divide by it rather than trapping.
  RF_CHECK_EQ(ratio(1u, 0u).apply_floor(1000u), 0ull);
  RF_CHECK_EQ(ratio(1u, 0u).apply_ceil(1000u), 0ull);

  // Large inputs whose product would overflow a naive implementation.
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  const std::uint64_t huge = (kMax / 2u) * 2u;
  RF_CHECK_EQ(ratio(1u, 2u).apply_floor(huge), huge / 2u);
  RF_CHECK_EQ(ratio(1u, 2u).apply_ceil(huge), huge / 2u);
  RF_CHECK_EQ(ratio(1u, 1u).apply_floor(huge), huge);
  RF_CHECK_EQ(ratio(1u, 4u).apply_floor(kMax), kMax / 4u);

  // satisfied_by() agrees with the exact comparison part * denominator >=
  // whole * numerator on values that fit.
  RF_CHECK(ratio(2u, 3u).satisfied_by(4u, 6u));
  RF_CHECK(ratio(2u, 3u).satisfied_by(2u, 3u));
  RF_CHECK(ratio(2u, 3u).satisfied_by(3u, 3u));
  RF_CHECK(!ratio(2u, 3u).satisfied_by(3u, 6u));
  RF_CHECK(ratio(0u, 1u).satisfied_by(0u, 1000u));
  RF_CHECK(ratio(1u, 1u).satisfied_by(1000u, 1000u));
  RF_CHECK(!ratio(1u, 1u).satisfied_by(999u, 1000u));

  for (const Ratio& value_ratio : ratios) {
    for (std::uint64_t part = 0; part <= 40; part += 4) {
      for (std::uint64_t whole = 0; whole <= 40; whole += 4) {
        const bool expected = whole == 0
                                  ? value_ratio.numerator == 0u
                                  : part * value_ratio.denominator >= whole * value_ratio.numerator;
        const bool actual = value_ratio.satisfied_by(part, whole);
        if (actual != expected) {
          RF_FAIL("satisfied_by(" + std::to_string(part) + ", " + std::to_string(whole) +
                  ") for " + value_ratio.to_string() + " was " + (actual ? "true" : "false") +
                  ", expected " + (expected ? "true" : "false"));
        }
      }
    }
  }

  // A whole of zero is only satisfied by a zero ratio.
  RF_CHECK(ratio(0u, 1u).satisfied_by(0u, 0u));
  RF_CHECK(!ratio(1u, 2u).satisfied_by(0u, 0u));
  RF_CHECK(!ratio(1u, 0u).satisfied_by(1u, 1u));  // an invalid ratio never passes

  // Boundary values whose products exceed 64 bits: exactly one half passes a
  // one-half threshold, one half minus one does not, and a whole input always
  // passes a three-quarter threshold.
  RF_CHECK(ratio(1u, 2u).satisfied_by(kMax, kMax));
  RF_CHECK(ratio(1u, 2u).satisfied_by(1ull << 63, kMax));
  RF_CHECK(!ratio(1u, 2u).satisfied_by((1ull << 63) - 1u, kMax));
  RF_CHECK(ratio(1u, 1u).satisfied_by(kMax, kMax));
  RF_CHECK(!ratio(3u, 4u).satisfied_by(kMax / 4u, kMax));
  RF_CHECK(ratio(3u, 4u).satisfied_by(kMax, kMax));
  RF_CHECK(ratio(0u, 1u).satisfied_by(0u, kMax));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

RF_TEST(lifecycle_rollout_transition_table) {
  check_transition_table(all_rollout_states(), rollout_table());

  // Every legal edge appears in the declared row and is never a self loop.
  const std::vector<RolloutState>& states = all_rollout_states();
  for (const RolloutState from : states) {
    for (const RolloutState to : states) {
      if (legal_transition(from, to)) {
        RF_CHECK(contains_state(rollout_table()[static_cast<std::size_t>(from)], to));
        RF_CHECK(from != to);
      }
    }
  }

  // Spot checks that read as the intended lifecycle.
  RF_CHECK(legal_transition(RolloutState::kCreated, RolloutState::kValidated));
  RF_CHECK(legal_transition(RolloutState::kValidated, RolloutState::kArmed));
  RF_CHECK(legal_transition(RolloutState::kArmed, RolloutState::kRunning));
  RF_CHECK(legal_transition(RolloutState::kRunning, RolloutState::kAdvancing));
  RF_CHECK(legal_transition(RolloutState::kAdvancing, RolloutState::kCompleted));
  RF_CHECK(legal_transition(RolloutState::kAborting, RolloutState::kRollingBack));
  RF_CHECK(legal_transition(RolloutState::kRollingBack, RolloutState::kCompleted));
  RF_CHECK(!legal_transition(RolloutState::kCreated, RolloutState::kRunning));
  RF_CHECK(!legal_transition(RolloutState::kCreated, RolloutState::kCreated));
  RF_CHECK(!legal_transition(RolloutState::kCompleted, RolloutState::kRunning));
  RF_CHECK(!legal_transition(RolloutState::kRetired, RolloutState::kCompleted));
  // Pause remembers where it came from: every interrupted state is a legal
  // resume target from kPaused.
  RF_CHECK(legal_transition(RolloutState::kPaused, RolloutState::kArmed));
  RF_CHECK(legal_transition(RolloutState::kPaused, RolloutState::kRunning));
  RF_CHECK(legal_transition(RolloutState::kPaused, RolloutState::kGated));
  RF_CHECK(legal_transition(RolloutState::kPaused, RolloutState::kSoaking));
  RF_CHECK(legal_transition(RolloutState::kPaused, RolloutState::kAdvancing));
}

RF_TEST(lifecycle_stage_transition_table) {
  check_transition_table(all_stage_states(), stage_table());

  const std::vector<StageState> terminal = {StageState::kSucceeded, StageState::kFailed,
                                            StageState::kAborted, StageState::kRolledBack,
                                            StageState::kSkipped};
  for (const StageState state : all_stage_states()) {
    RF_CHECK_EQ(is_terminal(state), contains_state(terminal, state));
  }

  // Terminal stage states only ever compensate, and the two sinks never move.
  RF_CHECK(legal_transition(StageState::kSucceeded, StageState::kRolledBack));
  RF_CHECK(legal_transition(StageState::kFailed, StageState::kRolledBack));
  RF_CHECK(legal_transition(StageState::kAborted, StageState::kRolledBack));
  for (const StageState target : all_stage_states()) {
    RF_CHECK(!legal_transition(StageState::kRolledBack, target));
    RF_CHECK(!legal_transition(StageState::kSkipped, target));
  }
  RF_CHECK(!legal_transition(StageState::kSucceeded, StageState::kRunning));
  RF_CHECK(!legal_transition(StageState::kPending, StageState::kRunning));
  RF_CHECK(!legal_transition(StageState::kBlocked, StageState::kRunning));
}

RF_TEST(lifecycle_target_transition_table) {
  check_transition_table(all_target_states(), target_table());

  const std::vector<TargetState> terminal = {
      TargetState::kSucceeded,  TargetState::kFailed,       TargetState::kCancelled,
      TargetState::kRolledBack, TargetState::kRollbackFailed, TargetState::kExhausted};
  for (const TargetState state : all_target_states()) {
    RF_CHECK_EQ(is_terminal(state), contains_state(terminal, state));
  }

  // A rolled back, rollback-failed or exhausted target never moves again.
  for (const TargetState target : all_target_states()) {
    RF_CHECK(!legal_transition(TargetState::kRolledBack, target));
    RF_CHECK(!legal_transition(TargetState::kRollbackFailed, target));
    RF_CHECK(!legal_transition(TargetState::kExhausted, target));
  }
  RF_CHECK(legal_transition(TargetState::kSucceeded, TargetState::kRollingBack));
  RF_CHECK(legal_transition(TargetState::kFailed, TargetState::kRollingBack));
  RF_CHECK(legal_transition(TargetState::kCancelled, TargetState::kRollingBack));
  RF_CHECK(legal_transition(TargetState::kFailed, TargetState::kExhausted));
  RF_CHECK(legal_transition(TargetState::kRollingBack, TargetState::kRolledBack));
  RF_CHECK(legal_transition(TargetState::kRollingBack, TargetState::kRollbackFailed));
  // Reconciliation may conclude that an unknown attempt never started.
  RF_CHECK(legal_transition(TargetState::kUnknown, TargetState::kPending));
  RF_CHECK(legal_transition(TargetState::kUnknown, TargetState::kDispatched));
  RF_CHECK(!legal_transition(TargetState::kUnknown, TargetState::kUnknown));
  RF_CHECK(!legal_transition(TargetState::kPending, TargetState::kSucceeded));
  RF_CHECK(!legal_transition(TargetState::kDispatched, TargetState::kDispatched));
}

RF_TEST(lifecycle_rollout_state_predicates) {
  const std::vector<RolloutState>& states = all_rollout_states();

  const std::vector<RolloutState> terminal = {RolloutState::kFailed, RolloutState::kCompleted,
                                              RolloutState::kRetired};
  const std::vector<RolloutState> dispatchable = {RolloutState::kRunning, RolloutState::kSoaking,
                                                  RolloutState::kAdvancing};
  const std::vector<RolloutState> pausable = {RolloutState::kArmed, RolloutState::kRunning,
                                              RolloutState::kGated, RolloutState::kSoaking,
                                              RolloutState::kAdvancing};
  const std::vector<RolloutState> suspended = {RolloutState::kPaused, RolloutState::kGated,
                                               RolloutState::kSoaking, RolloutState::kAborting,
                                               RolloutState::kRollingBack};

  for (const RolloutState state : states) {
    RF_CHECK_EQ(is_terminal(state), contains_state(terminal, state));
    RF_CHECK_EQ(admits_dispatch(state), contains_state(dispatchable, state));
    RF_CHECK_EQ(can_pause(state), contains_state(pausable, state));
    RF_CHECK_EQ(is_suspended(state), contains_state(suspended, state));

    // can_pause() agrees with the transition table: it is exactly the set of
    // states the table lets move into kPaused.
    RF_CHECK_EQ(can_pause(state), legal_transition(state, RolloutState::kPaused));
    RF_CHECK_EQ(contains_state(pausable, state), legal_transition(state, RolloutState::kPaused));

    // Dispatch is only possible in the active family, never before a rollout is
    // running and never after it has stopped.
    if (admits_dispatch(state)) {
      RF_CHECK(!is_terminal(state));
      RF_CHECK(legal_transition(state, RolloutState::kRunning) || state == RolloutState::kRunning);
    }

    // Terminal states have no outgoing transition other than kRetired, and they
    // can neither dispatch nor be paused. kRetired itself is the sink: it has no
    // outgoing edge at all, not even to itself.
    if (is_terminal(state)) {
      RF_CHECK(!admits_dispatch(state));
      RF_CHECK(!can_pause(state));
      for (const RolloutState target : states) {
        if (state == RolloutState::kRetired) {
          RF_CHECK(!legal_transition(state, target));
        } else {
          RF_CHECK_EQ(legal_transition(state, target), target == RolloutState::kRetired);
        }
      }
    }

    // Every state except the retired sink has at least one legal successor.
    bool has_successor = false;
    for (const RolloutState target : states) {
      if (legal_transition(state, target)) {
        has_successor = true;
      }
    }
    RF_CHECK_EQ(has_successor, state != RolloutState::kRetired);

    // The rendered name of a real state is never the unknown fallback.
    RF_CHECK(std::string(to_string(state)) != std::string("unknown"));
  }

  // The names are part of the persisted state machine.
  RF_CHECK_EQ(std::string(to_string(RolloutState::kCreated)), std::string("created"));
  RF_CHECK_EQ(std::string(to_string(RolloutState::kRollingBack)), std::string("rolling_back"));
  RF_CHECK_EQ(std::string(to_string(RolloutState::kRetired)), std::string("retired"));
  RF_CHECK_EQ(std::string(to_string(StageState::kRolledBack)), std::string("rolled_back"));
  RF_CHECK_EQ(std::string(to_string(StageState::kSkipped)), std::string("skipped"));
  RF_CHECK_EQ(std::string(to_string(TargetState::kRollbackFailed)), std::string("rollback_failed"));
  RF_CHECK_EQ(std::string(to_string(TargetState::kExhausted)), std::string("exhausted"));
  RF_CHECK_EQ(std::string(to_string(TargetState::kUnknown)), std::string("unknown"));
}

RF_TEST_MAIN()
