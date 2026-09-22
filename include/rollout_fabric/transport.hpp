// Rollout Fabric - framed transport over real TCP sockets.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The controller and its target workers are separate operating system
// processes that talk over loopback TCP. Frames are length prefixed, magic
// tagged, version tagged, sequence numbered, CRC-32 protected over the header
// and SHA-256 protected over the body, and bounded in both directions.
//
// The framing layer is where hostile input lands, so it is written to be
// total: a decoder never trusts a length, never allocates before it has checked
// the length against the configured ceiling, and never leaves a partially
// decoded frame observable to the layer above.
#pragma once

#include <cstdint>
#include <memory>
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

namespace rollout_fabric {

inline constexpr std::uint16_t kFrameVersion = 1;
inline constexpr std::uint32_t kFrameMagic = 0x31424652u;  // 'RFB1'
inline constexpr std::size_t kFrameHeaderBytes = 48;

enum class MessageType : std::uint16_t {
  kInvalid = 0,
  kHello = 1,
  kHelloAck = 2,
  kDispatch = 3,
  kDispatchAck = 4,
  kCancel = 5,
  kCancelAck = 6,
  kEvidence = 7,
  kHeartbeat = 8,
  kHeartbeatAck = 9,
  kReconcileRequest = 10,
  kReconcileResponse = 11,
  kShutdown = 12,
  kError = 13,
  kCommandRequest = 14,
  kCommandResponse = 15,
  kHealthRequest = 16,
  kHealthResponse = 17,
};

[[nodiscard]] std::string_view to_string(MessageType type) noexcept;

struct Frame {
  MessageType type = MessageType::kInvalid;
  std::uint32_t flags = 0;
  Sequence sequence{};
  std::vector<std::byte> body;
};

// Encodes one frame into 'out', appending. Never emits a frame whose body
// exceeds the configured ceiling.
[[nodiscard]] Status encode_frame(const Frame& frame, const RuntimeLimits& limits, std::vector<std::byte>& out);

// Incremental decoder. Bytes arrive in arbitrary chunks; complete frames come
// out in order. A protocol violation latches the decoder into a failed state
// and every subsequent feed returns the same error.
class FrameDecoder {
 public:
  explicit FrameDecoder(const RuntimeLimits& limits) : limits_(limits) {}

  [[nodiscard]] Status feed(std::span<const std::byte> data, std::vector<Frame>& out);

  [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size(); }
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] std::uint64_t frames_decoded() const noexcept { return frames_decoded_; }
  [[nodiscard]] const Status& failure() const noexcept { return failure_; }
  // A clean end of stream must not leave a partial frame behind.
  [[nodiscard]] Status expect_clean_end() const;
  void reset();

 private:
  [[nodiscard]] Status fail(Status status);

  RuntimeLimits limits_{};
  std::vector<std::byte> buffer_;
  bool failed_ = false;
  Status failure_ = Status::success();
  std::uint64_t frames_decoded_ = 0;
  std::uint64_t declared_total_ = 0;
};

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------

// Initialises the platform socket layer exactly once per process.
[[nodiscard]] Status ensure_socket_runtime();
void shutdown_socket_runtime() noexcept;

class Socket {
 public:
  Socket() = default;
  ~Socket();
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  [[nodiscard]] static Result<Socket> listen_loopback(std::uint16_t port);
  [[nodiscard]] static Result<Socket> connect_loopback(std::uint16_t port, Duration timeout);
  [[nodiscard]] Result<Socket> accept();
  [[nodiscard]] Status set_nonblocking(bool enabled);
  [[nodiscard]] Status set_no_delay(bool enabled);
  [[nodiscard]] Status close();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint16_t bound_port() const;
  [[nodiscard]] std::uintptr_t raw() const noexcept { return handle_; }

  // Non-blocking primitives. kUnavailable means "try again"; anything else is
  // terminal for the connection.
  [[nodiscard]] Result<std::size_t> recv(std::span<std::byte> out);
  [[nodiscard]] Result<std::size_t> send(std::span<const std::byte> data);
  [[nodiscard]] bool peer_closed() const noexcept { return peer_closed_; }

 private:
  void adopt(std::uintptr_t handle) noexcept;
  std::uintptr_t handle_ = static_cast<std::uintptr_t>(~std::uintptr_t{0});
  bool peer_closed_ = false;
};

// Waits until at least one socket is readable, writable or failed.
struct SelectResult {
  bool readable = false;
  bool writable = false;
  bool failed = false;
};

[[nodiscard]] Status select_one(const Socket& socket, bool want_write, Duration timeout, SelectResult& out);

// A bounded, framing-aware connection. Outbound frames are queued up to a
// configured ceiling and written when the socket reports writable; inbound
// bytes are decoded into at most max_queued_frames_per_connection frames.
class FrameChannel {
 public:
  FrameChannel() = default;
  FrameChannel(Socket socket, const RuntimeLimits& limits);

  FrameChannel(FrameChannel&&) noexcept = default;
  FrameChannel& operator=(FrameChannel&&) noexcept = default;

  [[nodiscard]] Status queue(const Frame& frame);
  [[nodiscard]] bool pump_out();
  [[nodiscard]] bool pump_in();
  // Drains the decoded inbound frames.
  [[nodiscard]] std::vector<Frame> take_inbound();
  [[nodiscard]] std::size_t inbound_backlog() const noexcept { return inbound_.size(); }
  [[nodiscard]] std::size_t outbound_backlog() const noexcept { return outbound_.size(); }
  [[nodiscard]] Status flush_out(Duration timeout);
  [[nodiscard]] bool closed() const noexcept { return closed_; }
  [[nodiscard]] const Status& failure() const noexcept { return failure_; }
  void close() noexcept;

  [[nodiscard]] const Socket& socket() const noexcept { return socket_; }
  [[nodiscard]] std::uint64_t frames_sent() const noexcept { return frames_sent_; }
  [[nodiscard]] std::uint64_t frames_received() const noexcept { return frames_received_; }

 private:
  [[nodiscard]] Status fail(Status status);

  Socket socket_{};
  FrameDecoder decoder_{default_limits()};
  RuntimeLimits limits_{};
  std::vector<Frame> inbound_;
  std::vector<Frame> outbound_;
  std::vector<std::byte> outbound_bytes_;
  std::size_t outbound_offset_ = 0;
  bool closed_ = false;
  Status failure_ = Status::success();
  std::uint64_t frames_sent_ = 0;
  std::uint64_t frames_received_ = 0;
};

// Concrete Transport used by the controller to talk to workers.
class INodeTransport {
 public:
  INodeTransport() = default;
  virtual ~INodeTransport();
  INodeTransport(const INodeTransport&) = delete;
  INodeTransport& operator=(const INodeTransport&) = delete;

  [[nodiscard]] virtual Status send(const Frame& frame) = 0;
  [[nodiscard]] virtual std::vector<Frame> poll() = 0;
  [[nodiscard]] virtual bool alive() const = 0;
  [[nodiscard]] virtual WorkerId peer() const = 0;
};

}  // namespace rollout_fabric
