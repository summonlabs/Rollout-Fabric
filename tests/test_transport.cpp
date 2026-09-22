// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Adversarial framing and loopback socket tests for the Rollout Fabric
// transport: header layout, incremental decoding, hostile header mutation,
// decoder latching, real TCP sockets and the bounded FrameChannel.
//
// There are no timeouts in this file. The only waits are the bounded select()
// readiness polls that the library itself performs (connect_loopback takes a
// connect deadline, and select_one takes a readiness wait); every wait loop
// here is additionally bounded by an attempt or round count, so a stuck socket
// fails the test rather than hanging the suite.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rollout_fabric/codec.hpp"
#include "rollout_fabric/digest.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"
#include "rollout_fabric/transport.hpp"
#include "test_harness.hpp"

using namespace rollout_fabric;

namespace {

// ---------------------------------------------------------------------------
// Byte level helpers: the tests read the header themselves rather than trusting
// the encoder's own accessors.
// ---------------------------------------------------------------------------

[[nodiscard]] std::span<const std::byte> as_bytes(std::string_view text) noexcept {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

[[nodiscard]] std::uint16_t read_u16_at(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(bytes[offset]) |
      (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8));
}

[[nodiscard]] std::uint32_t read_u32_at(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (index * 8);
  }
  return value;
}

[[nodiscard]] std::uint64_t read_u64_at(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (index * 8);
  }
  return value;
}

void write_u16_at(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(static_cast<std::uint8_t>(value & 0xFFu));
  bytes[offset + 1] = static_cast<std::byte>(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void write_u32_at(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned index = 0; index < 4; ++index) {
    bytes[offset + index] =
        static_cast<std::byte>(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu));
  }
}

// Recomputes the header CRC after a deliberate mutation, so that a test can
// reach the check that sits behind it.
void recompute_header_crc(std::vector<std::byte>& bytes) {
  const std::span<const std::byte> header(bytes.data(), kFrameHeaderBytes);
  write_u32_at(bytes, 40, crc32(header.first(40)));
}

// ---------------------------------------------------------------------------
// Framing helpers.
// ---------------------------------------------------------------------------

// A deliberately small ceiling keeps every framing test cheap; the checks are
// about behaviour at the boundary, not about megabyte payloads.
[[nodiscard]] RuntimeLimits frame_limits(std::uint32_t queue_ceiling) {
  RuntimeLimits limits = default_limits();
  limits.max_frame_bytes = 4096;
  limits.max_message_bytes = 4096;
  limits.max_queued_frames_per_connection = queue_ceiling;
  return limits;
}

[[nodiscard]] Frame make_frame(MessageType type, std::uint32_t flags, std::uint64_t sequence,
                               std::string_view body) {
  Frame frame;
  frame.type = type;
  frame.flags = flags;
  frame.sequence = Sequence{sequence};
  const std::span<const std::byte> view = as_bytes(body);
  frame.body.assign(view.begin(), view.end());
  return frame;
}

[[nodiscard]] std::vector<std::byte> frame_bytes(const Frame& frame, const RuntimeLimits& limits) {
  std::vector<std::byte> out;
  const Status status = encode_frame(frame, limits, out);
  if (!status.ok()) {
    RF_FAIL("encode_frame failed inside a test helper: " + status.message());
  }
  return out;
}

// Feeds a hostile byte stream and requires the decoder to reject it with
// kCorrupt, to latch, and to emit nothing at all.
void check_rejected(std::span<const std::byte> bytes, const RuntimeLimits& limits,
                    const char* expected_text) {
  FrameDecoder decoder(limits);
  std::vector<Frame> frames;
  const Status status = decoder.feed(bytes, frames);
  RF_CHECK(!status.ok());
  RF_CHECK(status.code() == StatusCode::kCorrupt);
  RF_CHECK(decoder.failed());
  RF_CHECK(frames.empty());
  RF_CHECK(status.message().find(expected_text) != std::string::npos);
  RF_CHECK(decoder.failure() == status);

  // The latch holds: further feeds keep returning the original failure, even
  // when the bytes that follow are valid.
  std::vector<Frame> later;
  const std::vector<std::byte> valid = frame_bytes(
      make_frame(MessageType::kHeartbeat, 0u, 1ull, "after the failure"), limits);
  const Status again = decoder.feed(std::span<const std::byte>(valid), later);
  RF_CHECK(again == status);
  RF_CHECK(later.empty());
  RF_CHECK(decoder.expect_clean_end() == status);
}

// ---------------------------------------------------------------------------
// Socket helpers.
// ---------------------------------------------------------------------------

struct SocketPair {
  Socket listener;
  Socket client;
  Socket server;
  std::uint16_t port = 0;
};

// Bounded readiness poll: at most 'attempts' select_one() waits of 10 ms. This
// is a readiness wait, not a test timeout - the loop returns as soon as the
// socket is ready, and it is used only where the library documents a readiness
// query.
[[nodiscard]] bool wait_readable(const Socket& socket, int attempts) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    SelectResult selected{};
    const Status status = select_one(socket, false, Duration::from_millis(10), selected);
    if (!status.ok()) {
      RF_FAIL("select_one failed while waiting for readability: " + status.message());
      return false;
    }
    if (selected.readable || selected.failed) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool wait_writable(const Socket& socket, int attempts) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    SelectResult selected{};
    const Status status = select_one(socket, true, Duration::from_millis(10), selected);
    if (!status.ok()) {
      RF_FAIL("select_one failed while waiting for writability: " + status.message());
      return false;
    }
    if (selected.writable || selected.failed) {
      return true;
    }
  }
  return false;
}

// A connected loopback pair plus the listener that produced it. accept() is
// only called once select_one() has reported the listener readable: the
// listening socket is blocking, so the readiness wait is what keeps the call
// from blocking, not a test timeout.
[[nodiscard]] std::optional<SocketPair> make_loopback_pair() {
  const Status runtime = ensure_socket_runtime();
  if (!runtime.ok()) {
    RF_FAIL("ensure_socket_runtime failed: " + runtime.message());
    return std::nullopt;
  }

  auto listener_result = Socket::listen_loopback(0);
  if (!listener_result.ok()) {
    RF_FAIL("listen_loopback(0) failed: " + listener_result.status().message());
    return std::nullopt;
  }
  Socket listener = std::move(listener_result).value();
  const std::uint16_t port = listener.bound_port();
  if (port == 0) {
    RF_FAIL("listen_loopback(0) did not report a bound port");
    return std::nullopt;
  }

  // The connect deadline below is the library's own parameter; on loopback the
  // handshake completes immediately.
  auto client_result = Socket::connect_loopback(port, Duration::from_seconds(10));
  if (!client_result.ok()) {
    RF_FAIL("connect_loopback failed: " + client_result.status().message());
    return std::nullopt;
  }
  Socket client = std::move(client_result).value();

  if (!wait_readable(listener, 500)) {
    RF_FAIL("the listening socket never became readable after a connect");
    return std::nullopt;
  }
  auto accepted = listener.accept();
  if (!accepted.ok()) {
    RF_FAIL("accept failed: " + accepted.status().message());
    return std::nullopt;
  }
  Socket server = std::move(accepted).value();

  SocketPair pair;
  pair.listener = std::move(listener);
  pair.client = std::move(client);
  pair.server = std::move(server);
  pair.port = port;
  return pair;
}

// Moves frames from one channel to another with repeated non-blocking pumps.
// Both ends are driven from this single thread, so the loop alternates between
// them; the select_one() waits inside are bounded readiness polls, and the
// round ceiling keeps a stuck socket from hanging the suite.
[[nodiscard]] bool pump_until(FrameChannel& source, FrameChannel& sink, std::size_t wanted) {
  for (int round = 0; round < 20000; ++round) {
    if (sink.inbound_backlog() >= wanted) {
      return true;
    }
    // One non-blocking read and one non-blocking write per round. The read
    // happens first so that a full source socket buffer is drained by the
    // peer's receive window opening up.
    const bool read_now = sink.pump_in();
    if (!sink.failure().ok()) {
      RF_FAIL("pump_in failed: " + sink.failure().message());
      return false;
    }
    const bool drained = source.pump_out();
    if (!source.failure().ok()) {
      RF_FAIL("pump_out failed: " + source.failure().message());
      return false;
    }
    if (!read_now && !drained) {
      // Neither end moved: the source still has bytes the socket will not take
      // yet and the sink has nothing buffered. Wait (bounded) for either end to
      // become ready; these are readiness polls, not timeouts.
      if (source.closed() || sink.closed()) {
        RF_FAIL("a channel closed with frames still outstanding");
        return false;
      }
      const bool readable = wait_readable(sink.socket(), 20);
      const bool writable = wait_writable(source.socket(), 20);
      if (!readable && !writable) {
        RF_FAIL("neither end of the channel became ready");
        return false;
      }
    }
  }
  return sink.inbound_backlog() >= wanted;
}

}  // namespace

// ---------------------------------------------------------------------------
// Frame encoding
// ---------------------------------------------------------------------------

RF_TEST(transport_encode_frame_layout) {
  const RuntimeLimits limits = frame_limits(64);
  RF_CHECK_EQ(kFrameHeaderBytes, std::size_t{48});
  RF_CHECK_EQ(kFrameVersion, static_cast<std::uint16_t>(1));

  const std::vector<std::byte> body = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                       std::byte{0xEF}};
  Frame frame;
  frame.type = MessageType::kHeartbeat;
  frame.flags = 0x01020304u;
  frame.sequence = Sequence{0x0102030405060708ull};
  frame.body = body;

  std::vector<std::byte> out;
  const Status encoded = encode_frame(frame, limits, out);
  RF_REQUIRE(encoded.ok());

  // Exactly one header plus the body.
  RF_CHECK_EQ(out.size(), kFrameHeaderBytes + body.size());
  const std::span<const std::byte> bytes(out);

  // The documented header offsets.
  RF_CHECK_EQ(read_u32_at(bytes, 0), kFrameMagic);
  RF_CHECK_EQ(read_u16_at(bytes, 4), kFrameVersion);
  RF_CHECK_EQ(read_u16_at(bytes, 6), static_cast<std::uint16_t>(MessageType::kHeartbeat));
  RF_CHECK_EQ(read_u32_at(bytes, 8), 0x01020304u);
  RF_CHECK_EQ(read_u64_at(bytes, 12), 0x0102030405060708ull);
  RF_CHECK_EQ(read_u32_at(bytes, 20), static_cast<std::uint32_t>(body.size()));
  RF_CHECK_EQ(read_u32_at(bytes, 44), 0u);

  // The body digest at [24, 40) is the truncated SHA-256 of the body, and the
  // body itself is appended verbatim.
  const Digest128 expected_digest = truncate128(sha256(std::span<const std::byte>(body)));
  RF_CHECK(std::equal(out.begin() + 24, out.begin() + 40, expected_digest.begin()));
  RF_CHECK(std::equal(out.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes), out.end(),
                      body.begin()));

  // The header CRC at offset 40 covers exactly [0, 40).
  RF_CHECK_EQ(read_u32_at(bytes, 40), crc32(bytes.first(40)));
  RF_CHECK(read_u32_at(bytes, 40) != crc32(bytes.first(44)));
  RF_CHECK(read_u32_at(bytes, 40) != 0u);

  // encode_frame() appends and never truncates what was already there.
  std::vector<std::byte> appended = {std::byte{0xAA}, std::byte{0xBB}};
  const Status appended_status = encode_frame(frame, limits, appended);
  RF_REQUIRE(appended_status.ok());
  RF_CHECK_EQ(appended.size(), std::size_t{2} + kFrameHeaderBytes + body.size());
  RF_CHECK(appended[0] == std::byte{0xAA});
  RF_CHECK(appended[1] == std::byte{0xBB});
  RF_CHECK(std::equal(appended.begin() + 2, appended.end(), out.begin()));

  // An empty body is legal and produces a bare header.
  const Frame empty = make_frame(MessageType::kShutdown, 0u, 0ull, "");
  std::vector<std::byte> bare;
  const Status bare_status = encode_frame(empty, limits, bare);
  RF_REQUIRE(bare_status.ok());
  RF_CHECK_EQ(bare.size(), kFrameHeaderBytes);
  RF_CHECK_EQ(read_u32_at(std::span<const std::byte>(bare), 20), 0u);
  RF_CHECK_EQ(read_u32_at(std::span<const std::byte>(bare), 44), 0u);

  // Two encodings of the same frame are byte identical.
  std::vector<std::byte> again;
  const Status again_status = encode_frame(frame, limits, again);
  RF_REQUIRE(again_status.ok());
  RF_CHECK(again == out);
}

RF_TEST(transport_encode_frame_ceiling) {
  const RuntimeLimits limits = frame_limits(64);
  const std::size_t ceiling = static_cast<std::size_t>(limits.max_frame_bytes);

  // One byte over the ceiling is refused, and the output buffer is untouched.
  const std::string too_big(ceiling + 1u, 'x');
  const Frame oversized = make_frame(MessageType::kEvidence, 0u, 1ull, too_big);
  std::vector<std::byte> out = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
  const Status refused = encode_frame(oversized, limits, out);
  RF_CHECK(!refused.ok());
  RF_CHECK(refused.code() == StatusCode::kLimitExceeded);
  RF_CHECK(refused.message().find("exceeds") != std::string::npos);
  RF_REQUIRE(out.size() == 3u);
  RF_CHECK(out[0] == std::byte{0x11});
  RF_CHECK(out[1] == std::byte{0x22});
  RF_CHECK(out[2] == std::byte{0x33});

  // Exactly at the ceiling is accepted.
  const std::string at_ceiling(ceiling, 'x');
  std::vector<std::byte> accepted;
  RF_CHECK(encode_frame(make_frame(MessageType::kEvidence, 0u, 1ull, at_ceiling), limits, accepted).ok());
  RF_CHECK_EQ(accepted.size(), kFrameHeaderBytes + ceiling);

  // The ceiling is inclusive: a one byte ceiling accepts a one byte body and
  // refuses a two byte one, leaving the output buffer untouched.
  RuntimeLimits tiny = limits;
  tiny.max_frame_bytes = 1;
  std::vector<std::byte> at_one = {std::byte{0x99}};
  const Status one_status = encode_frame(make_frame(MessageType::kHello, 0u, 0ull, "z"), tiny, at_one);
  RF_CHECK(one_status.ok());
  RF_CHECK_EQ(at_one.size(), std::size_t{1} + kFrameHeaderBytes + 1u);
  std::vector<std::byte> over_one = {std::byte{0x99}};
  const Status over_status = encode_frame(make_frame(MessageType::kHello, 0u, 0ull, "yz"), tiny, over_one);
  RF_CHECK(!over_status.ok());
  RF_CHECK(over_status.code() == StatusCode::kLimitExceeded);
  RF_REQUIRE(over_one.size() == 1u);
  RF_CHECK(over_one[0] == std::byte{0x99});
}

// ---------------------------------------------------------------------------
// Frame decoding
// ---------------------------------------------------------------------------

RF_TEST(transport_decoder_incremental_and_batched) {
  const RuntimeLimits limits = frame_limits(64);
  const Frame first = make_frame(MessageType::kHello, 0x11u, 7ull, "alpha");
  const Frame second = make_frame(MessageType::kEvidence, 0x22u, 8ull, "bravo-body");
  const std::vector<std::byte> bytes_first = frame_bytes(first, limits);
  const std::vector<std::byte> bytes_second = frame_bytes(second, limits);

  // Two complete frames fed in two calls.
  FrameDecoder sequential(limits);
  std::vector<Frame> sequential_frames;
  const Status first_feed =
      sequential.feed(std::span<const std::byte>(bytes_first), sequential_frames);
  RF_REQUIRE(first_feed.ok());
  RF_CHECK_EQ(sequential_frames.size(), std::size_t{1});
  const Status second_feed =
      sequential.feed(std::span<const std::byte>(bytes_second), sequential_frames);
  RF_REQUIRE(second_feed.ok());
  RF_REQUIRE(sequential_frames.size() == 2);
  RF_CHECK_EQ(sequential.frames_decoded(), 2ull);
  RF_CHECK_EQ(sequential.buffered(), std::size_t{0});
  RF_CHECK(sequential.expect_clean_end().ok());

  // Both frames in one buffer.
  std::vector<std::byte> combined = bytes_first;
  combined.insert(combined.end(), bytes_second.begin(), bytes_second.end());
  FrameDecoder batched(limits);
  std::vector<Frame> batched_frames;
  const Status batched_feed = batched.feed(std::span<const std::byte>(combined), batched_frames);
  RF_REQUIRE(batched_feed.ok());
  RF_REQUIRE(batched_frames.size() == 2);
  RF_CHECK_EQ(batched.frames_decoded(), 2ull);
  RF_CHECK_EQ(batched.buffered(), std::size_t{0});
  RF_CHECK(batched.expect_clean_end().ok());

  // The same stream one byte at a time.
  FrameDecoder trickled(limits);
  std::vector<Frame> trickled_frames;
  for (const std::byte value : combined) {
    const std::byte single = value;
    const Status fed = trickled.feed(std::span<const std::byte>(&single, 1), trickled_frames);
    RF_REQUIRE(fed.ok());
  }
  RF_REQUIRE(trickled_frames.size() == 2);
  RF_CHECK_EQ(trickled.frames_decoded(), 2ull);
  RF_CHECK_EQ(trickled.buffered(), std::size_t{0});
  RF_CHECK(trickled.expect_clean_end().ok());

  // All three decoders produced the same frames.
  for (std::size_t index = 0; index < 2; ++index) {
    RF_CHECK(sequential_frames[index].type == batched_frames[index].type);
    RF_CHECK(sequential_frames[index].type == trickled_frames[index].type);
    RF_CHECK_EQ(sequential_frames[index].flags, batched_frames[index].flags);
    RF_CHECK_EQ(sequential_frames[index].flags, trickled_frames[index].flags);
    RF_CHECK_EQ(value_of(sequential_frames[index].sequence), value_of(batched_frames[index].sequence));
    RF_CHECK_EQ(value_of(sequential_frames[index].sequence), value_of(trickled_frames[index].sequence));
    RF_CHECK(sequential_frames[index].body == batched_frames[index].body);
    RF_CHECK(sequential_frames[index].body == trickled_frames[index].body);
  }
  RF_CHECK(sequential_frames[0].type == MessageType::kHello);
  RF_CHECK(sequential_frames[1].type == MessageType::kEvidence);
  RF_CHECK_EQ(sequential_frames[0].flags, 0x11u);
  RF_CHECK_EQ(sequential_frames[1].flags, 0x22u);
  RF_CHECK_EQ(value_of(sequential_frames[0].sequence), 7ull);
  RF_CHECK_EQ(value_of(sequential_frames[1].sequence), 8ull);
  RF_CHECK(sequential_frames[0].body == first.body);
  RF_CHECK(sequential_frames[1].body == second.body);

  // Feeding nothing at all is harmless.
  std::vector<Frame> nothing;
  RF_CHECK(trickled.feed(std::span<const std::byte>(), nothing).ok());
  RF_CHECK(nothing.empty());

  // The two frames concatenated in the opposite call order still decode, and
  // an empty frame in the middle produces three frames.
  const std::vector<std::byte> bytes_empty = frame_bytes(make_frame(MessageType::kHeartbeat, 0u, 9ull, ""), limits);
  std::vector<std::byte> mixed = bytes_first;
  mixed.insert(mixed.end(), bytes_empty.begin(), bytes_empty.end());
  mixed.insert(mixed.end(), bytes_second.begin(), bytes_second.end());
  FrameDecoder three(limits);
  std::vector<Frame> three_frames;
  // Feed in awkward chunks: 1, 47, 48, 49, then the rest.
  const std::vector<std::size_t> chunks = {1, 47, 48, 49, 1000000};
  std::size_t offset = 0;
  for (const std::size_t chunk : chunks) {
    if (offset >= mixed.size()) {
      break;
    }
    const std::size_t take = std::min(chunk, mixed.size() - offset);
    const Status fed = three.feed(std::span<const std::byte>(mixed).subspan(offset, take), three_frames);
    RF_REQUIRE(fed.ok());
    offset += take;
  }
  RF_REQUIRE(three_frames.size() == 3);
  RF_CHECK_EQ(three.frames_decoded(), 3ull);
  RF_CHECK(three_frames[0].body == first.body);
  RF_CHECK(three_frames[1].body.empty());
  RF_CHECK(three_frames[2].body == second.body);
  RF_CHECK(three.expect_clean_end().ok());
}

RF_TEST(transport_decoder_frame_at_ceiling) {
  const RuntimeLimits limits = frame_limits(64);
  const std::size_t ceiling = static_cast<std::size_t>(limits.max_frame_bytes);

  // A body of exactly max_frame_bytes is the largest frame the decoder accepts.
  const std::string body(ceiling, 'y');
  const Frame frame = make_frame(MessageType::kCommandResponse, 0u, 42ull, body);
  const std::vector<std::byte> bytes = frame_bytes(frame, limits);
  RF_CHECK_EQ(bytes.size(), kFrameHeaderBytes + ceiling);

  FrameDecoder decoder(limits);
  std::vector<Frame> frames;
  const Status fed = decoder.feed(std::span<const std::byte>(bytes), frames);
  RF_REQUIRE(fed.ok());
  RF_REQUIRE(frames.size() == 1);
  RF_CHECK_EQ(frames[0].body.size(), ceiling);
  RF_CHECK(frames[0].body == frame.body);
  RF_CHECK(frames[0].type == MessageType::kCommandResponse);
  RF_CHECK(decoder.expect_clean_end().ok());

  // A frame one byte larger is refused by the encoder, and a hand-built header
  // that declares it is rejected by the decoder. The status code is kCorrupt,
  // not kLimitExceeded: the length check reports hostile framing rather than a
  // local limit, and it runs before the header CRC is verified.
  std::vector<std::byte> oversized = bytes;
  write_u32_at(oversized, 20, static_cast<std::uint32_t>(ceiling + 1u));
  check_rejected(std::span<const std::byte>(oversized), limits, "exceeds the configured ceiling");

  // The largest declared length a hostile peer could ask for is refused too.
  std::vector<std::byte> enormous = bytes;
  write_u32_at(enormous, 20, 0xFFFFFFFFu);
  check_rejected(std::span<const std::byte>(enormous), limits, "exceeds the configured ceiling");
}

RF_TEST(transport_decoder_rejects_corruption) {
  const RuntimeLimits limits = frame_limits(64);
  const Frame frame = make_frame(MessageType::kDispatch, 1u, 0x0102030405060708ull, "payload-bytes");
  const std::vector<std::byte> good = frame_bytes(frame, limits);

  // Sanity: the pristine frame decodes.
  {
    FrameDecoder decoder(limits);
    std::vector<Frame> frames;
    const Status fed = decoder.feed(std::span<const std::byte>(good), frames);
    RF_REQUIRE(fed.ok());
    RF_REQUIRE(frames.size() == 1);
    RF_CHECK(frames[0].body == frame.body);
  }

  // Magic, offset 0.
  std::vector<std::byte> magic = good;
  magic[0] ^= std::byte{0x01};
  check_rejected(std::span<const std::byte>(magic), limits, "magic");

  // Version, offset 4.
  std::vector<std::byte> version = good;
  write_u16_at(version, 4, static_cast<std::uint16_t>(kFrameVersion + 1));
  check_rejected(std::span<const std::byte>(version), limits, "version");
  std::vector<std::byte> zero_version = good;
  write_u16_at(zero_version, 4, 0u);
  check_rejected(std::span<const std::byte>(zero_version), limits, "version");

  // Type, offset 6: zero and an unassigned value are both unknown types.
  std::vector<std::byte> zero_type = good;
  write_u16_at(zero_type, 6, 0u);
  check_rejected(std::span<const std::byte>(zero_type), limits, "type");
  std::vector<std::byte> unknown_type = good;
  write_u16_at(unknown_type, 6, 0xFFFFu);
  check_rejected(std::span<const std::byte>(unknown_type), limits, "type");

  // Flags, offset 8: covered by the header CRC.
  std::vector<std::byte> flags = good;
  flags[8] ^= std::byte{0x80};
  check_rejected(std::span<const std::byte>(flags), limits, "CRC");

  // Sequence, offset 12: covered by the header CRC.
  std::vector<std::byte> sequence = good;
  sequence[12] ^= std::byte{0x01};
  check_rejected(std::span<const std::byte>(sequence), limits, "CRC");

  // Body length, offset 20: a shorter declared length with a repaired CRC moves
  // the failure to the body digest, because the digest still describes the real
  // body.
  std::vector<std::byte> shortened = good;
  write_u32_at(shortened, 20, 1u);
  recompute_header_crc(shortened);
  check_rejected(std::span<const std::byte>(shortened), limits, "digest");

  std::vector<std::byte> lengthened = good;
  write_u32_at(lengthened, 20, static_cast<std::uint32_t>(frame.body.size() + 1u));
  recompute_header_crc(lengthened);
  // The decoder waits for the byte that never arrives, so this is a truncated
  // stream rather than a corrupted one.
  {
    FrameDecoder decoder(limits);
    std::vector<Frame> frames;
    const Status fed = decoder.feed(std::span<const std::byte>(lengthened), frames);
    RF_REQUIRE(fed.ok());
    RF_CHECK(frames.empty());
    RF_CHECK(!decoder.expect_clean_end().ok());
    RF_CHECK(!decoder.failed());
  }

  // Body digest, offsets 24..40: the CRC covers it, so a bare flip is caught by
  // the CRC; with the CRC repaired the digest comparison catches it.
  std::vector<std::byte> digest_crc = good;
  digest_crc[24] ^= std::byte{0x01};
  check_rejected(std::span<const std::byte>(digest_crc), limits, "CRC");
  std::vector<std::byte> digest = good;
  digest[24] ^= std::byte{0x01};
  recompute_header_crc(digest);
  check_rejected(std::span<const std::byte>(digest), limits, "body digest");

  // A flipped body byte leaves the header valid and is caught by the digest.
  std::vector<std::byte> body_flip = good;
  body_flip[kFrameHeaderBytes] ^= std::byte{0x01};
  check_rejected(std::span<const std::byte>(body_flip), limits, "body digest");

  // A byte appended after a complete frame is trailing garbage rather than a
  // digest failure: the declared frame still decodes, and expect_clean_end()
  // reports the leftover byte.
  {
    FrameDecoder decoder(limits);
    std::vector<Frame> frames;
    std::vector<std::byte> trailing = good;
    trailing.push_back(std::byte{0x00});
    const Status fed = decoder.feed(std::span<const std::byte>(trailing), frames);
    RF_REQUIRE(fed.ok());
    RF_REQUIRE(frames.size() == 1);
    RF_CHECK(frames[0].body == frame.body);
    RF_CHECK_EQ(decoder.buffered(), std::size_t{1});
    const Status clean = decoder.expect_clean_end();
    RF_CHECK(!clean.ok());
    RF_CHECK(clean.code() == StatusCode::kCorrupt);
    RF_CHECK(clean.message().find("incomplete") != std::string::npos);
  }

  // Header CRC, offset 40.
  std::vector<std::byte> header_crc = good;
  header_crc[40] ^= std::byte{0x01};
  check_rejected(std::span<const std::byte>(header_crc), limits, "CRC");

  // Reserved field, offset 44: it is outside the CRC coverage on purpose, and a
  // non-zero value is rejected outright.
  std::vector<std::byte> reserved = good;
  write_u32_at(reserved, 44, 1u);
  check_rejected(std::span<const std::byte>(reserved), limits, "reserved");
  RF_CHECK(crc32(std::span<const std::byte>(reserved).first(40)) == read_u32_at(reserved, 40));

  // A truncated header and a truncated body are not decode failures; they are
  // incomplete streams, reported by expect_clean_end().
  {
    FrameDecoder decoder(limits);
    std::vector<Frame> frames;
    const Status partial =
        decoder.feed(std::span<const std::byte>(good).first(kFrameHeaderBytes - 1), frames);
    RF_REQUIRE(partial.ok());
    RF_CHECK(frames.empty());
    RF_CHECK(!decoder.failed());
    RF_CHECK_EQ(decoder.buffered(), kFrameHeaderBytes - 1);
    const Status clean = decoder.expect_clean_end();
    RF_CHECK(!clean.ok());
    RF_CHECK(clean.code() == StatusCode::kCorrupt);
    RF_CHECK(clean.message().find("incomplete") != std::string::npos);

    // reset() clears the partial buffer and the decoder keeps working.
    decoder.reset();
    RF_CHECK_EQ(decoder.buffered(), std::size_t{0});
    RF_CHECK(decoder.expect_clean_end().ok());
    const Status recovered = decoder.feed(std::span<const std::byte>(good), frames);
    RF_REQUIRE(recovered.ok());
    RF_CHECK_EQ(frames.size(), std::size_t{1});
  }

  // A latched decoder fails identically on later feeds until it is reset.
  {
    FrameDecoder decoder(limits);
    std::vector<Frame> frames;
    std::vector<std::byte> bad = good;
    bad[0] ^= std::byte{0x01};
    const Status first_failure = decoder.feed(std::span<const std::byte>(bad), frames);
    RF_REQUIRE(!first_failure.ok());
    RF_CHECK(decoder.failed());
    RF_CHECK_EQ(decoder.frames_decoded(), 0ull);

    const Status second_failure = decoder.feed(std::span<const std::byte>(good), frames);
    RF_CHECK(!second_failure.ok());
    RF_CHECK(second_failure == first_failure);
    RF_CHECK(decoder.failure() == first_failure);
    RF_CHECK(frames.empty());
    RF_CHECK(decoder.expect_clean_end() == first_failure);

    decoder.reset();
    RF_CHECK(!decoder.failed());
    RF_CHECK(decoder.failure().ok());
    RF_CHECK(decoder.expect_clean_end().ok());
    RF_CHECK_EQ(decoder.frames_decoded(), 0ull);
    const Status recovered = decoder.feed(std::span<const std::byte>(good), frames);
    RF_REQUIRE(recovered.ok());
    RF_CHECK_EQ(frames.size(), std::size_t{1});
    RF_CHECK_EQ(decoder.frames_decoded(), 1ull);
  }
}

// ---------------------------------------------------------------------------
// Real loopback sockets
// ---------------------------------------------------------------------------

RF_TEST(transport_socket_loopback_roundtrip) {
  // The socket runtime is reference counted and idempotent.
  RF_CHECK(ensure_socket_runtime().ok());

  std::optional<SocketPair> pair = make_loopback_pair();
  RF_REQUIRE(pair.has_value());
  RF_CHECK(pair->port != 0);
  RF_CHECK(pair->listener.valid());
  RF_CHECK(pair->client.valid());
  RF_CHECK(pair->server.valid());
  RF_CHECK_EQ(pair->client.bound_port() != 0, true);
  Socket& client = pair->client;
  Socket& server = pair->server;

  // Both ends are non-blocking and delay free.
  RF_REQUIRE(client.set_nonblocking(true).ok());
  RF_REQUIRE(server.set_nonblocking(true).ok());
  RF_REQUIRE(client.set_no_delay(true).ok());
  RF_REQUIRE(server.set_no_delay(true).ok());

  // Nothing is pending yet, so a bounded readiness wait reports "not readable"
  // and select_one() returns success without setting any flag.
  SelectResult idle{};
  RF_REQUIRE(select_one(server, false, Duration::from_millis(20), idle).ok());
  RF_CHECK(!idle.readable);
  RF_CHECK(!idle.writable);
  RF_CHECK(!idle.failed);

  // Empty transfers are no-ops, not errors.
  std::array<std::byte, 8> scratch{};
  const auto empty_recv = server.recv(std::span<std::byte>());
  RF_REQUIRE(empty_recv.ok());
  RF_CHECK_EQ(empty_recv.value(), std::size_t{0});
  const auto empty_send = client.send(std::span<const std::byte>());
  RF_REQUIRE(empty_send.ok());
  RF_CHECK_EQ(empty_send.value(), std::size_t{0});

  // A small payload: send, wait for readability, receive.
  const std::vector<std::byte> ping = {std::byte{'p'}, std::byte{'i'}, std::byte{'n'},
                                       std::byte{'g'}};
  const auto sent = client.send(std::span<const std::byte>(ping));
  RF_REQUIRE(sent.ok());
  RF_CHECK_EQ(sent.value(), ping.size());

  SelectResult readable{};
  RF_REQUIRE(select_one(server, false, Duration::from_millis(2000), readable).ok());
  RF_CHECK(readable.readable);
  RF_CHECK(!readable.failed);
  const auto received = server.recv(std::span<std::byte>(scratch));
  RF_REQUIRE(received.ok());
  RF_CHECK_EQ(received.value(), ping.size());
  RF_CHECK(std::equal(ping.begin(), ping.end(), scratch.begin()));

  // A poll (negative duration) returns immediately without waiting.
  SelectResult polled{};
  RF_REQUIRE(select_one(server, false, Duration::from_millis(-1), polled).ok());
  RF_CHECK(!polled.readable);

  // Reading a non-blocking socket with nothing pending reports kUnavailable and
  // does not latch anything.
  const auto empty_read = server.recv(std::span<std::byte>(scratch));
  RF_CHECK(!empty_read.ok());
  RF_CHECK(empty_read.status().code() == StatusCode::kUnavailable);
  RF_CHECK(!server.peer_closed());

  // A large payload moved with non-blocking sends and receives. Both ends are
  // driven from this thread, so the loop alternates between them. Each recv
  // asks for at most 16 KiB, so the transfer necessarily spans several recv
  // calls; the select_one() waits are bounded readiness polls, and the round
  // ceiling keeps a stuck socket from hanging the suite.
  constexpr std::size_t kRecvChunk = 16u * 1024u;
  const std::size_t payload_size = 512u * 1024u;
  std::vector<std::byte> payload(payload_size);
  for (std::size_t index = 0; index < payload.size(); ++index) {
    payload[index] = static_cast<std::byte>(static_cast<std::uint8_t>((index * 31u + 7u) % 251u));
  }
  std::vector<std::byte> received_payload(payload_size);
  std::size_t sent_total = 0;
  std::size_t received_total = 0;
  int rounds = 0;
  constexpr int kRoundCeiling = 200000;
  while ((sent_total < payload.size() || received_total < payload.size()) &&
         rounds < kRoundCeiling) {
    ++rounds;
    bool progressed = false;
    if (sent_total < payload.size()) {
      const auto chunk = client.send(std::span<const std::byte>(payload).subspan(sent_total));
      if (!chunk.ok()) {
        if (chunk.status().code() != StatusCode::kUnavailable) {
          RF_FAIL("send failed: " + chunk.status().message());
          break;
        }
      } else if (chunk.value() > 0) {
        sent_total += chunk.value();
        progressed = true;
      }
    }
    if (received_total < payload.size()) {
      const std::size_t want = std::min(kRecvChunk, payload.size() - received_total);
      const auto chunk =
          server.recv(std::span<std::byte>(received_payload).subspan(received_total, want));
      if (!chunk.ok()) {
        if (chunk.status().code() != StatusCode::kUnavailable) {
          RF_FAIL("recv failed: " + chunk.status().message());
          break;
        }
      } else if (chunk.value() > 0) {
        received_total += chunk.value();
        progressed = true;
      }
    }
    if (!progressed) {
      // Both directions are blocked on each other: wait for either end to make
      // progress. These are bounded readiness waits inside the code under test.
      RF_REQUIRE(wait_writable(client, 200));
      RF_REQUIRE(wait_readable(server, 200));
    }
  }
  RF_CHECK(rounds < kRoundCeiling);
  RF_CHECK_EQ(sent_total, payload.size());
  RF_CHECK_EQ(received_total, payload.size());
  RF_CHECK(received_payload == payload);
  // The payload could not have arrived in a single recv: each call asked for at
  // most 16 KiB, so at least 32 calls were needed.
  RF_CHECK(rounds >= static_cast<int>(payload_size / kRecvChunk));

  // Peer close latches peer_closed_ on the other end. Closing first, then
  // waiting for readability, means recv() sees the end of stream rather than a
  // would-block.
  RF_REQUIRE(client.close().ok());
  RF_CHECK(!client.valid());
  RF_REQUIRE(wait_readable(server, 500));
  const auto closed_read = server.recv(std::span<std::byte>(scratch));
  RF_CHECK(!closed_read.ok());
  RF_CHECK(closed_read.status().code() == StatusCode::kUnavailable);
  RF_CHECK(server.peer_closed());

  // Operating on a closed socket is refused, not undefined.
  SelectResult after_close{};
  RF_CHECK(!select_one(client, false, Duration::from_millis(1), after_close).ok());
  const auto after_close_send = client.send(std::span<const std::byte>(ping));
  RF_CHECK(!after_close_send.ok());
  RF_CHECK(after_close_send.status().code() == StatusCode::kUnavailable);
  RF_CHECK(!client.set_nonblocking(true).ok());
  RF_CHECK(!client.accept().ok());
  RF_CHECK_EQ(client.bound_port(), static_cast<std::uint16_t>(0));
  RF_REQUIRE(server.close().ok());
  RF_REQUIRE(server.close().ok());  // closing twice is idempotent
  RF_REQUIRE(pair->listener.close().ok());

  // A default constructed socket is invalid and refuses every operation.
  Socket invalid;
  RF_CHECK(!invalid.valid());
  RF_CHECK_EQ(invalid.bound_port(), static_cast<std::uint16_t>(0));
  RF_CHECK(!invalid.set_nonblocking(true).ok());
  RF_CHECK(!invalid.set_no_delay(true).ok());
  RF_CHECK(!invalid.accept().ok());
  RF_CHECK(!invalid.recv(std::span<std::byte>(scratch)).ok());
  RF_REQUIRE(invalid.close().ok());
  SelectResult invalid_selected{};
  RF_CHECK(!select_one(invalid, false, Duration::from_millis(1), invalid_selected).ok());
}

// ---------------------------------------------------------------------------
// FrameChannel
// ---------------------------------------------------------------------------

RF_TEST(transport_frame_channel_queue_ceiling) {
  const RuntimeLimits limits = frame_limits(4);
  std::optional<SocketPair> pair = make_loopback_pair();
  RF_REQUIRE(pair.has_value());
  FrameChannel channel(std::move(pair->client), limits);

  RF_CHECK(!channel.closed());
  RF_CHECK(channel.failure().ok());
  RF_CHECK_EQ(channel.outbound_backlog(), std::size_t{0});
  RF_CHECK_EQ(channel.inbound_backlog(), std::size_t{0});

  // The queue accepts exactly the configured number of frames.
  const Frame frame = make_frame(MessageType::kHeartbeat, 0u, 1ull, "beat");
  for (std::uint32_t index = 0; index < limits.max_queued_frames_per_connection; ++index) {
    const Status queued_frame = channel.queue(frame);
    RF_REQUIRE(queued_frame.ok());
  }
  RF_CHECK_EQ(channel.outbound_backlog(),
              static_cast<std::size_t>(limits.max_queued_frames_per_connection));

  // One more is refused with kResourceExhausted, and the refusal does not
  // poison the channel or drop what is already queued.
  const Status refused = channel.queue(frame);
  RF_CHECK(!refused.ok());
  RF_CHECK(refused.code() == StatusCode::kResourceExhausted);
  RF_CHECK(refused.message().find("queue") != std::string::npos);
  RF_CHECK(channel.failure().ok());
  RF_CHECK(!channel.closed());
  RF_CHECK_EQ(channel.outbound_backlog(),
              static_cast<std::size_t>(limits.max_queued_frames_per_connection));

  // The queued frames still drain. flush_out() only needs the peer's socket
  // buffers to accept the bytes, not the peer to read them.
  const Status channel_flushed = channel.flush_out(Duration::from_seconds(5));
  RF_REQUIRE(channel_flushed.ok());
  RF_CHECK_EQ(channel.outbound_backlog(), std::size_t{0});
  RF_CHECK_EQ(channel.frames_sent(), 4ull);
}

RF_TEST(transport_frame_channel_pumps_both_ways) {
  const RuntimeLimits limits = frame_limits(16);
  std::optional<SocketPair> pair = make_loopback_pair();
  RF_REQUIRE(pair.has_value());
  FrameChannel left(std::move(pair->client), limits);
  FrameChannel right(std::move(pair->server), limits);

  const Frame first = make_frame(MessageType::kHello, 0x1u, 1ull, "one");
  const Frame second = make_frame(MessageType::kEvidence, 0x2u, 2ull, "two-two");
  const Frame third = make_frame(MessageType::kHeartbeat, 0x3u, 3ull, "");

  // left -> right.
  const Status queued_first = left.queue(first);
  RF_REQUIRE(queued_first.ok());
  const Status queued_second = left.queue(second);
  RF_REQUIRE(queued_second.ok());
  const Status queued_third = left.queue(third);
  RF_REQUIRE(queued_third.ok());
  const bool forwarded_three = pump_until(left, right, 3);
  RF_REQUIRE(forwarded_three);
  RF_CHECK_EQ(left.outbound_backlog(), std::size_t{0});
  RF_CHECK_EQ(left.frames_sent(), 3ull);
  RF_CHECK_EQ(right.frames_received(), 3ull);
  RF_CHECK_EQ(right.inbound_backlog(), std::size_t{3});

  const std::vector<Frame> forwarded = right.take_inbound();
  RF_REQUIRE(forwarded.size() == 3);
  RF_CHECK(forwarded[0].type == MessageType::kHello);
  RF_CHECK(forwarded[1].type == MessageType::kEvidence);
  RF_CHECK(forwarded[2].type == MessageType::kHeartbeat);
  RF_CHECK_EQ(forwarded[0].flags, 0x1u);
  RF_CHECK_EQ(forwarded[1].flags, 0x2u);
  RF_CHECK_EQ(forwarded[2].flags, 0x3u);
  RF_CHECK_EQ(value_of(forwarded[0].sequence), 1ull);
  RF_CHECK_EQ(value_of(forwarded[1].sequence), 2ull);
  RF_CHECK_EQ(value_of(forwarded[2].sequence), 3ull);
  RF_CHECK(forwarded[0].body == first.body);
  RF_CHECK(forwarded[1].body == second.body);
  RF_CHECK(forwarded[2].body.empty());
  RF_CHECK_EQ(right.inbound_backlog(), std::size_t{0});
  RF_CHECK(right.take_inbound().empty());

  // right -> left, in the other direction and in the other order.
  const Status back_second = right.queue(second);
  RF_REQUIRE(back_second.ok());
  const Status back_first = right.queue(first);
  RF_REQUIRE(back_first.ok());
  const bool returned_two = pump_until(right, left, 2);
  RF_REQUIRE(returned_two);
  RF_CHECK_EQ(right.outbound_backlog(), std::size_t{0});
  RF_CHECK_EQ(right.frames_sent(), 2ull);
  RF_CHECK_EQ(left.frames_received(), 2ull);
  const std::vector<Frame> returned = left.take_inbound();
  RF_REQUIRE(returned.size() == 2);
  RF_CHECK(returned[0].body == second.body);
  RF_CHECK(returned[1].body == first.body);
  RF_CHECK(left.take_inbound().empty());

  // pump_out() with nothing queued is a no-op that reports success and sends
  // nothing new.
  const std::uint64_t sent_before = left.frames_sent();
  RF_CHECK(left.pump_out());
  RF_CHECK_EQ(left.frames_sent(), sent_before);

  // Closing is explicit and idempotent, and a closed channel refuses to queue.
  left.close();
  RF_CHECK(left.closed());
  const Status closed_queue = left.queue(first);
  RF_CHECK(!closed_queue.ok());
  RF_CHECK(closed_queue.code() == StatusCode::kUnavailable);
  left.close();
  RF_CHECK(left.closed());
}

RF_TEST(transport_frame_channel_flush_out_deadline) {
  // The sender has a 600 frame queue; the receiver has a much larger ceiling so
  // that its inbound backlog check can never fire while this test drains it.
  const RuntimeLimits sender_limits = frame_limits(600);
  const RuntimeLimits receiver_limits = frame_limits(4096);
  std::optional<SocketPair> pair = make_loopback_pair();
  RF_REQUIRE(pair.has_value());
  FrameChannel left(std::move(pair->client), sender_limits);
  FrameChannel right(std::move(pair->server), receiver_limits);

  // A small batch drains completely.
  const Frame small = make_frame(MessageType::kHeartbeat, 0u, 1ull, "drain-me");
  const Status queued_small_one = left.queue(small);
  RF_REQUIRE(queued_small_one.ok());
  const Status queued_small_two = left.queue(small);
  RF_REQUIRE(queued_small_two.ok());
  const Status small_flushed = left.flush_out(Duration::from_seconds(5));
  RF_REQUIRE(small_flushed.ok());
  RF_CHECK_EQ(left.outbound_backlog(), std::size_t{0});
  RF_CHECK_EQ(left.frames_sent(), 2ull);
  const bool crossed = pump_until(left, right, 2);
  RF_REQUIRE(crossed);
  RF_CHECK_EQ(right.take_inbound().size(), std::size_t{2});

  // Now a backlog far larger than the loopback socket buffers, with a peer that
  // never reads: flush_out() must give up with kDeadlineExceeded instead of
  // spinning or growing without bound. 600 frames of 4 KiB is about 2.4 MiB,
  // roughly twenty times the default socket buffering.
  const std::size_t queued = 600;
  const std::string payload(static_cast<std::size_t>(sender_limits.max_frame_bytes), 'z');
  const Frame big = make_frame(MessageType::kEvidence, 0u, 9ull, payload);
  for (std::size_t index = 0; index < queued; ++index) {
    const Status queued_big = left.queue(big);
    RF_REQUIRE(queued_big.ok());
  }
  RF_CHECK_EQ(left.outbound_backlog(), queued);

  const Status stalled = left.flush_out(Duration::from_millis(250));
  RF_CHECK(!stalled.ok());
  RF_CHECK(stalled.code() == StatusCode::kDeadlineExceeded);
  RF_CHECK(stalled.message().find("still queued") != std::string::npos);
  // A deadline is not a channel failure: the socket is still open and the
  // unsent frames are still queued.
  RF_CHECK(left.failure().ok());
  RF_CHECK(!left.closed());
  RF_CHECK(left.outbound_backlog() > 0);
  RF_CHECK(left.outbound_backlog() < queued);
  RF_CHECK(left.frames_sent() > 2ull);

  // A peer that reads lets the whole backlog drain, and every frame arrives
  // intact.
  const bool drained = pump_until(left, right, queued);
  RF_REQUIRE(drained);
  RF_CHECK_EQ(left.outbound_backlog(), std::size_t{0});
  const std::vector<Frame> big_frames = right.take_inbound();
  RF_REQUIRE(big_frames.size() == queued);
  RF_CHECK(big_frames.front().body == big.body);
  RF_CHECK(big_frames.back().body == big.body);
  RF_CHECK_EQ(right.frames_received(), 602ull);  // the two small frames plus 600 big ones
  RF_CHECK_EQ(value_of(big_frames.front().sequence), 9ull);
  RF_CHECK(right.failure().ok());
}

RF_TEST_MAIN()
