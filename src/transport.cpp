// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/transport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "decode_util.hpp"

namespace rollout_fabric {
namespace {

// Sockets live in uintptr_t fields; this is Winsock's INVALID_SOCKET value.
constexpr std::uintptr_t kInvalidHandle = static_cast<std::uintptr_t>(INVALID_SOCKET);

// Upper bound on the bytes a single pump_in() read asks for. The decoder is the
// component that knows the real frame ceiling, so this only keeps one read from
// being unbounded.
constexpr std::size_t kReadChunkBytes = 16u * 1024u;

[[nodiscard]] SOCKET raw_socket(std::uintptr_t handle) noexcept {
  return static_cast<SOCKET>(handle);
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

[[nodiscard]] Status winsock_unavailable(const char* action) {
  return make_status(StatusCode::kUnavailable,
                     std::string(action) + " failed with Winsock error " +
                         std::to_string(WSAGetLastError()));
}

[[nodiscard]] Status winsock_failure(StatusCode code, const char* action, int error) {
  return make_status(code, std::string(action) + " failed with Winsock error " +
                               std::to_string(error));
}

// Terminal connection errors: the peer is gone, so the socket is marked closed
// and the caller is told to stop using it.
[[nodiscard]] bool is_connection_lost(int error) noexcept {
  return error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENOTCONN ||
         error == WSAESHUTDOWN;
}

// The process-wide socket runtime. The mutex protects the reference count only;
// it is never held while a socket is created, read from or written to.
struct SocketRuntime {
  std::mutex mutex;
  int references = 0;
};

[[nodiscard]] SocketRuntime& socket_runtime() {
  static SocketRuntime runtime;
  return runtime;
}

[[nodiscard]] bool socket_runtime_active() {
  std::lock_guard<std::mutex> guard(socket_runtime().mutex);
  return socket_runtime().references > 0;
}

// Brings the runtime up when the caller has not done so explicitly, without
// inflating the reference count of a caller that has.
[[nodiscard]] Status require_socket_runtime() {
  if (socket_runtime_active()) {
    return Status::success();
  }
  return ensure_socket_runtime();
}

[[nodiscard]] timeval to_timeval(Duration timeout) noexcept {
  timeval value{};
  if (timeout.is_negative()) {
    return value;  // poll
  }
  const std::int64_t seconds = timeout.nanos() / 1000000000;
  const std::int64_t micros = (timeout.nanos() % 1000000000) / 1000;
  value.tv_sec = static_cast<long>(seconds > static_cast<std::int64_t>(LONG_MAX)
                                       ? static_cast<std::int64_t>(LONG_MAX)
                                       : seconds);
  value.tv_usec = static_cast<long>(micros);
  return value;
}

}  // namespace

std::string_view to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::kInvalid: return "invalid";
    case MessageType::kHello: return "hello";
    case MessageType::kHelloAck: return "hello_ack";
    case MessageType::kDispatch: return "dispatch";
    case MessageType::kDispatchAck: return "dispatch_ack";
    case MessageType::kCancel: return "cancel";
    case MessageType::kCancelAck: return "cancel_ack";
    case MessageType::kEvidence: return "evidence";
    case MessageType::kHeartbeat: return "heartbeat";
    case MessageType::kHeartbeatAck: return "heartbeat_ack";
    case MessageType::kReconcileRequest: return "reconcile_request";
    case MessageType::kReconcileResponse: return "reconcile_response";
    case MessageType::kShutdown: return "shutdown";
    case MessageType::kError: return "error";
    case MessageType::kCommandRequest: return "command_request";
    case MessageType::kCommandResponse: return "command_response";
    case MessageType::kHealthRequest: return "health_request";
    case MessageType::kHealthResponse: return "health_response";
  }
  return "unknown";
}

// The header is exactly kFrameHeaderBytes long:
//
//   0  magic            u32
//   4  version          u16
//   6  type             u16
//   8  flags            u32
//  12  sequence         u64
//  20  body_length      u32
//  24  body_digest      16 bytes (truncate128(sha256(body)))
//  40  header_crc       u32 (CRC-32 over bytes [0, 40))
//  44  reserved         u32 (zero; a non-zero value is rejected)
//
// All integers are little endian, which is what the canonical codec emits.
Status encode_frame(const Frame& frame, const RuntimeLimits& limits, std::vector<std::byte>& out) {
  if (frame.body.size() > limits.max_frame_bytes) {
    return make_status(StatusCode::kLimitExceeded,
                       "frame body of " + std::to_string(frame.body.size()) +
                           " byte(s) exceeds the configured ceiling of " +
                           std::to_string(limits.max_frame_bytes) + " byte(s)");
  }

  ByteWriter writer;
  writer.reserve(kFrameHeaderBytes + frame.body.size());
  writer.u32(kFrameMagic);
  writer.u16(kFrameVersion);
  writer.u16(static_cast<std::uint16_t>(frame.type));
  writer.u32(frame.flags);
  writer.u64(value_of(frame.sequence));
  writer.u32(static_cast<std::uint32_t>(frame.body.size()));
  const Digest128 body_digest = truncate128(sha256(std::span<const std::byte>(frame.body)));
  writer.raw(body_digest);
  writer.u32(crc32(writer.span()));  // CRC-32 over the preceding 40 header bytes
  writer.u32(0u);                    // reserved

  out.insert(out.end(), writer.data().begin(), writer.data().end());
  out.insert(out.end(), frame.body.begin(), frame.body.end());
  return Status::success();
}

Status FrameDecoder::feed(std::span<const std::byte> data, std::vector<Frame>& out) {
  if (failed_) {
    return failure_;
  }

  const auto parse_header = [this]() -> Status {
    const std::span<const std::byte> header =
        std::span<const std::byte>(buffer_).first(kFrameHeaderBytes);
    const std::uint32_t magic = read_u32_at(header, 0);
    if (magic != kFrameMagic) {
      return fail(make_status(StatusCode::kCorrupt,
                              "frame magic mismatch: this stream is not Rollout Fabric framing"));
    }
    const std::uint16_t version = read_u16_at(header, 4);
    if (version != kFrameVersion) {
      return fail(make_status(StatusCode::kCorrupt,
                              "unsupported frame version " + std::to_string(version) +
                                  " (this build speaks version " +
                                  std::to_string(kFrameVersion) + ")"));
    }
    const std::uint16_t raw_type = read_u16_at(header, 6);
    if (raw_type <= static_cast<std::uint16_t>(MessageType::kInvalid) ||
        raw_type > static_cast<std::uint16_t>(MessageType::kHealthResponse)) {
      return fail(make_status(StatusCode::kCorrupt,
                              "unknown message type " + std::to_string(raw_type)));
    }
    const std::uint32_t body_length = read_u32_at(header, 20);
    if (body_length > limits_.max_frame_bytes) {
      return fail(make_status(StatusCode::kCorrupt,
                              "declared frame body of " + std::to_string(body_length) +
                                  " byte(s) exceeds the configured ceiling of " +
                                  std::to_string(limits_.max_frame_bytes) + " byte(s)"));
    }
    const std::uint32_t expected_crc = read_u32_at(header, 40);
    if (expected_crc != crc32(header.first(40))) {
      return fail(make_status(StatusCode::kCorrupt, "frame header CRC mismatch"));
    }
    const std::uint32_t reserved = read_u32_at(header, 44);
    if (reserved != 0u) {
      return fail(make_status(StatusCode::kCorrupt,
                              "frame header reserved field is " + std::to_string(reserved) +
                                  ", this build requires zero"));
    }
    declared_total_ = static_cast<std::uint64_t>(kFrameHeaderBytes) + body_length;
    return Status::success();
  };

  const auto emit_frame = [this, &out]() -> Status {
    const std::size_t total = static_cast<std::size_t>(declared_total_);
    const std::span<const std::byte> header =
        std::span<const std::byte>(buffer_).first(kFrameHeaderBytes);
    const std::span<const std::byte> body = std::span<const std::byte>(buffer_).subspan(
        kFrameHeaderBytes, total - kFrameHeaderBytes);
    Digest128 declared{};
    for (std::size_t index = 0; index < kDigest128Bytes; ++index) {
      declared[index] = header[24 + index];
    }
    const Digest128 actual = truncate128(sha256(body));
    if (!digest_equal(declared, actual)) {
      return fail(make_status(StatusCode::kCorrupt, "frame body digest mismatch"));
    }

    Frame frame;
    frame.type = static_cast<MessageType>(read_u16_at(header, 6));
    frame.flags = read_u32_at(header, 8);
    frame.sequence = Sequence{read_u64_at(header, 12)};
    frame.body.assign(body.begin(), body.end());
    out.push_back(std::move(frame));

    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(total));
    declared_total_ = 0;
    ++frames_decoded_;
    return Status::success();
  };

  // Every iteration either parses a complete header, emits a complete frame, or
  // consumes at least one byte, so the loop always terminates. A frame that
  // happens to end exactly on a chunk boundary is emitted before the input runs
  // out, which is what makes one byte at a time decoding work.
  std::size_t offset = 0;
  for (;;) {
    if (declared_total_ == 0 && buffer_.size() >= kFrameHeaderBytes) {
      RF_TRY(parse_header());
    }
    if (declared_total_ != 0 && buffer_.size() >= static_cast<std::size_t>(declared_total_)) {
      RF_TRY(emit_frame());
      continue;  // another complete frame may already be buffered
    }
    if (offset >= data.size()) {
      break;  // the rest of the stream has not arrived yet
    }
    const std::size_t want =
        (buffer_.size() < kFrameHeaderBytes)
            ? (kFrameHeaderBytes - buffer_.size())
            : (static_cast<std::size_t>(declared_total_) - buffer_.size());
    const std::size_t available = data.size() - offset;
    const std::size_t take = (available < want) ? available : want;
    buffer_.insert(buffer_.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                   data.begin() + static_cast<std::ptrdiff_t>(offset + take));
    offset += take;
  }
  return Status::success();
}

Status FrameDecoder::fail(Status status) {
  if (!failed_) {
    failed_ = true;
    failure_ = std::move(status);
    declared_total_ = 0;
    buffer_.clear();
  }
  return failure_;
}

Status FrameDecoder::expect_clean_end() const {
  if (failed_) {
    return failure_;
  }
  if (!buffer_.empty()) {
    return make_status(StatusCode::kCorrupt,
                       "the stream ended with " + std::to_string(buffer_.size()) +
                           " byte(s) of an incomplete frame buffered");
  }
  return Status::success();
}

void FrameDecoder::reset() {
  buffer_.clear();
  failed_ = false;
  failure_ = Status::success();
  frames_decoded_ = 0;
  declared_total_ = 0;
}

// ---------------------------------------------------------------------------
// Socket runtime
// ---------------------------------------------------------------------------

Status ensure_socket_runtime() {
  SocketRuntime& runtime = socket_runtime();
  std::lock_guard<std::mutex> guard(runtime.mutex);
  if (runtime.references > 0) {
    ++runtime.references;
    return Status::success();
  }
  WSADATA data{};
  const int result = WSAStartup(MAKEWORD(2, 2), &data);
  if (result != 0) {
    return make_status(StatusCode::kUnavailable,
                       "WSAStartup failed with code " + std::to_string(result));
  }
  runtime.references = 1;
  return Status::success();
}

void shutdown_socket_runtime() noexcept {
  SocketRuntime& runtime = socket_runtime();
  std::lock_guard<std::mutex> guard(runtime.mutex);
  if (runtime.references == 0) {
    return;
  }
  --runtime.references;
  if (runtime.references == 0) {
    WSACleanup();
  }
}

// ---------------------------------------------------------------------------
// Socket
// ---------------------------------------------------------------------------

Socket::~Socket() {
  (void)close();
}

Socket::Socket(Socket&& other) noexcept
    : handle_(other.handle_), peer_closed_(other.peer_closed_) {
  other.handle_ = kInvalidHandle;
  other.peer_closed_ = false;
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    if (handle_ != kInvalidHandle) {
      ::closesocket(raw_socket(handle_));
    }
    handle_ = other.handle_;
    peer_closed_ = other.peer_closed_;
    other.handle_ = kInvalidHandle;
    other.peer_closed_ = false;
  }
  return *this;
}

void Socket::adopt(std::uintptr_t handle) noexcept {
  if (handle_ != kInvalidHandle) {
    ::closesocket(raw_socket(handle_));
  }
  handle_ = handle;
  peer_closed_ = false;
}

Result<Socket> Socket::listen_loopback(std::uint16_t port) {
  RF_TRY(require_socket_runtime());

  const SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == INVALID_SOCKET) {
    return winsock_unavailable("socket");
  }
  Socket listener;
  listener.adopt(static_cast<std::uintptr_t>(handle));

  const BOOL reuse = TRUE;
  if (::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                   static_cast<int>(sizeof(reuse))) == SOCKET_ERROR) {
    return winsock_unavailable("setsockopt(SO_REUSEADDR)");
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(handle, reinterpret_cast<const sockaddr*>(&address),
             static_cast<int>(sizeof(address))) == SOCKET_ERROR) {
    return winsock_unavailable("bind");
  }
  if (::listen(handle, SOMAXCONN) == SOCKET_ERROR) {
    return winsock_unavailable("listen");
  }
  return Result<Socket>(std::move(listener));
}

Result<Socket> Socket::connect_loopback(std::uint16_t port, Duration timeout) {
  RF_TRY(require_socket_runtime());

  const SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == INVALID_SOCKET) {
    return winsock_unavailable("socket");
  }
  Socket client;
  client.adopt(static_cast<std::uintptr_t>(handle));
  RF_TRY(client.set_nonblocking(true));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const int connected =
      ::connect(handle, reinterpret_cast<const sockaddr*>(&address), static_cast<int>(sizeof(address)));
  if (connected == SOCKET_ERROR) {
    const int error = WSAGetLastError();
    const bool in_progress = error == WSAEWOULDBLOCK || error == WSAEINPROGRESS ||
                             error == WSAEALREADY || error == WSAEINTR;
    if (!in_progress && error != WSAEISCONN) {
      return winsock_failure(StatusCode::kUnavailable, "connect", error);
    }
    if (in_progress) {
      SelectResult selected{};
      RF_TRY(select_one(client, true, timeout, selected));
      if (!selected.writable && !selected.failed) {
        return make_status(StatusCode::kDeadlineExceeded,
                           "connect to loopback port " + std::to_string(port) +
                               " did not complete within the requested timeout");
      }
      int pending = 0;
      int length = static_cast<int>(sizeof(pending));
      if (::getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&pending), &length) ==
          SOCKET_ERROR) {
        return winsock_unavailable("getsockopt(SO_ERROR)");
      }
      if (pending != 0) {
        return winsock_failure(StatusCode::kUnavailable, "connect", pending);
      }
    }
  }
  RF_TRY(client.set_nonblocking(false));
  return Result<Socket>(std::move(client));
}

Result<Socket> Socket::accept() {
  if (!valid()) {
    return make_status(StatusCode::kUnavailable, "accept: the listening socket is not open");
  }
  const SOCKET accepted = ::accept(raw_socket(handle_), nullptr, nullptr);
  if (accepted == INVALID_SOCKET) {
    const int error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK) {
      return make_status(StatusCode::kUnavailable, "accept: no connection is pending");
    }
    return winsock_failure(StatusCode::kUnavailable, "accept", error);
  }
  Socket peer;
  peer.adopt(static_cast<std::uintptr_t>(accepted));
  // Accepted sockets are handed out in blocking mode; callers that want to poll
  // ask for it explicitly (FrameChannel does).
  RF_TRY(peer.set_nonblocking(false));
  return Result<Socket>(std::move(peer));
}

Status Socket::set_nonblocking(bool enabled) {
  if (!valid()) {
    return make_status(StatusCode::kUnavailable, "set_nonblocking: the socket is not open");
  }
  u_long mode = enabled ? 1u : 0u;
  if (::ioctlsocket(raw_socket(handle_), FIONBIO, &mode) == SOCKET_ERROR) {
    return winsock_unavailable("ioctlsocket(FIONBIO)");
  }
  return Status::success();
}

Status Socket::set_no_delay(bool enabled) {
  if (!valid()) {
    return make_status(StatusCode::kUnavailable, "set_no_delay: the socket is not open");
  }
  const BOOL value = enabled ? TRUE : FALSE;
  if (::setsockopt(raw_socket(handle_), IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(value))) ==
      SOCKET_ERROR) {
    return winsock_unavailable("setsockopt(TCP_NODELAY)");
  }
  return Status::success();
}

Status Socket::close() {
  if (handle_ == kInvalidHandle) {
    return Status::success();
  }
  const int result = ::closesocket(raw_socket(handle_));
  handle_ = kInvalidHandle;
  if (result == SOCKET_ERROR) {
    return winsock_unavailable("closesocket");
  }
  return Status::success();
}

bool Socket::valid() const noexcept {
  return handle_ != kInvalidHandle;
}

std::uint16_t Socket::bound_port() const {
  if (!valid()) {
    return 0;
  }
  sockaddr_in address{};
  int length = static_cast<int>(sizeof(address));
  if (::getsockname(raw_socket(handle_), reinterpret_cast<sockaddr*>(&address), &length) ==
      SOCKET_ERROR) {
    return 0;
  }
  return ntohs(address.sin_port);
}

Result<std::size_t> Socket::recv(std::span<std::byte> out) {
  if (!valid()) {
    return make_status(StatusCode::kUnavailable, "recv: the socket is not open");
  }
  if (out.empty()) {
    return std::size_t{0};
  }
  const std::size_t requested =
      out.size() > static_cast<std::size_t>(INT_MAX) ? static_cast<std::size_t>(INT_MAX) : out.size();
  const int received = ::recv(raw_socket(handle_), reinterpret_cast<char*>(out.data()),
                              static_cast<int>(requested), 0);
  if (received == SOCKET_ERROR) {
    const int error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK) {
      return make_status(StatusCode::kUnavailable, "recv: no data is available");
    }
    if (is_connection_lost(error)) {
      peer_closed_ = true;
      return winsock_failure(StatusCode::kUnavailable, "recv (the peer is gone)", error);
    }
    return winsock_failure(StatusCode::kIoError, "recv", error);
  }
  if (received == 0) {
    peer_closed_ = true;
    return make_status(StatusCode::kUnavailable, "recv: the peer closed the connection");
  }
  return static_cast<std::size_t>(received);
}

Result<std::size_t> Socket::send(std::span<const std::byte> data) {
  if (!valid()) {
    return make_status(StatusCode::kUnavailable, "send: the socket is not open");
  }
  if (data.empty()) {
    return std::size_t{0};
  }
  const std::size_t requested =
      data.size() > static_cast<std::size_t>(INT_MAX) ? static_cast<std::size_t>(INT_MAX) : data.size();
  const int sent = ::send(raw_socket(handle_), reinterpret_cast<const char*>(data.data()),
                          static_cast<int>(requested), 0);
  if (sent == SOCKET_ERROR) {
    const int error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK) {
      return make_status(StatusCode::kUnavailable, "send: the socket buffer is full");
    }
    if (is_connection_lost(error)) {
      peer_closed_ = true;
      return winsock_failure(StatusCode::kUnavailable, "send (the peer is gone)", error);
    }
    return winsock_failure(StatusCode::kIoError, "send", error);
  }
  return static_cast<std::size_t>(sent);
}

Status select_one(const Socket& socket, bool want_write, Duration timeout, SelectResult& out) {
  out = SelectResult{};
  if (!socket.valid()) {
    return make_status(StatusCode::kUnavailable, "select_one: the socket is not open");
  }
  const SOCKET handle = raw_socket(socket.raw());
  fd_set read_set;
  fd_set write_set;
  fd_set error_set;
  FD_ZERO(&read_set);
  FD_ZERO(&write_set);
  FD_ZERO(&error_set);
  if (want_write) {
    FD_SET(handle, &write_set);
  } else {
    FD_SET(handle, &read_set);
  }
  FD_SET(handle, &error_set);

  timeval wait = to_timeval(timeout);
  const int ready = ::select(0, &read_set, &write_set, &error_set, &wait);
  if (ready == SOCKET_ERROR) {
    return winsock_unavailable("select");
  }
  if (ready == 0) {
    return Status::success();  // timed out; every flag stays false
  }
  out.readable = FD_ISSET(handle, &read_set) != 0;
  out.writable = FD_ISSET(handle, &write_set) != 0;
  out.failed = FD_ISSET(handle, &error_set) != 0;
  return Status::success();
}

// ---------------------------------------------------------------------------
// FrameChannel
// ---------------------------------------------------------------------------

FrameChannel::FrameChannel(Socket socket, const RuntimeLimits& limits)
    : socket_(std::move(socket)), decoder_(limits), limits_(limits) {
  if (!socket_.valid()) {
    (void)fail(make_status(StatusCode::kUnavailable, "the channel socket is not open"));
    return;
  }
  const Status non_blocking = socket_.set_nonblocking(true);
  if (!non_blocking.ok()) {
    (void)fail(non_blocking);
  }
}

Status FrameChannel::queue(const Frame& frame) {
  if (!failure_.ok()) {
    return failure_;
  }
  if (closed_ || !socket_.valid()) {
    return make_status(StatusCode::kUnavailable, "queue: the channel is closed");
  }
  if (outbound_.size() >= limits_.max_queued_frames_per_connection) {
    return make_status(StatusCode::kResourceExhausted,
                       "queue: the outbound queue already holds " +
                           std::to_string(outbound_.size()) + " frame(s), the configured ceiling");
  }
  outbound_.push_back(frame);
  return Status::success();
}

bool FrameChannel::pump_out() {
  if (!failure_.ok() || closed_ || !socket_.valid()) {
    return outbound_.empty() && outbound_bytes_.size() == outbound_offset_;
  }
  for (;;) {
    if (outbound_bytes_.size() == outbound_offset_) {
      if (outbound_.empty()) {
        outbound_bytes_.clear();
        outbound_offset_ = 0;
        return true;
      }
      outbound_bytes_.clear();
      outbound_offset_ = 0;
      const Status encoded = encode_frame(outbound_.front(), limits_, outbound_bytes_);
      if (!encoded.ok()) {
        (void)fail(encoded);
        return false;
      }
    }
    while (outbound_offset_ < outbound_bytes_.size()) {
      const auto written =
          socket_.send(std::span<const std::byte>(outbound_bytes_).subspan(outbound_offset_));
      if (!written.ok()) {
        if (written.status().code() == StatusCode::kUnavailable && !socket_.peer_closed()) {
          return false;  // the socket is full; try again when it is writable
        }
        (void)fail(written.status());
        return false;
      }
      if (written.value() == 0) {
        return false;
      }
      outbound_offset_ += written.value();
    }
    outbound_bytes_.clear();
    outbound_offset_ = 0;
    outbound_.erase(outbound_.begin());
    ++frames_sent_;
  }
}

bool FrameChannel::pump_in() {
  if (!failure_.ok() || closed_ || !socket_.valid()) {
    return false;
  }
  std::array<std::byte, kReadChunkBytes> buffer{};
  const auto received = socket_.recv(std::span<std::byte>(buffer.data(), buffer.size()));
  if (!received.ok()) {
    if (received.status().code() == StatusCode::kUnavailable && !socket_.peer_closed()) {
      return false;  // nothing to read right now
    }
    (void)fail(received.status());
    return false;
  }
  if (received.value() == 0) {
    return false;
  }
  const std::size_t before = inbound_.size();
  const Status fed = decoder_.feed(std::span<const std::byte>(buffer.data(), received.value()), inbound_);
  if (!fed.ok()) {
    (void)fail(fed);
    return false;
  }
  frames_received_ += static_cast<std::uint64_t>(inbound_.size() - before);
  if (inbound_.size() > limits_.max_queued_frames_per_connection) {
    (void)fail(make_status(StatusCode::kResourceExhausted,
                           "the inbound frame backlog exceeded the configured ceiling of " +
                               std::to_string(limits_.max_queued_frames_per_connection) +
                               " frame(s)"));
    return false;
  }
  return true;
}

std::vector<Frame> FrameChannel::take_inbound() {
  std::vector<Frame> frames = std::move(inbound_);
  inbound_.clear();
  return frames;
}

Status FrameChannel::flush_out(Duration timeout) {
  if (!failure_.ok()) {
    return failure_;
  }
  if (closed_ || !socket_.valid()) {
    return make_status(StatusCode::kUnavailable, "flush_out: the channel is closed");
  }
  const auto started = std::chrono::steady_clock::now();
  const auto budget = std::chrono::nanoseconds(timeout.is_negative() ? 0 : timeout.nanos());
  for (;;) {
    if (pump_out()) {
      return Status::success();
    }
    if (!failure_.ok()) {
      return failure_;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (elapsed >= budget) {
      return make_status(StatusCode::kDeadlineExceeded,
                         "flush_out timed out with " + std::to_string(outbound_.size()) +
                             " frame(s) still queued");
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::nanoseconds>(budget - elapsed).count();
    SelectResult selected{};
    RF_TRY(select_one(socket_, true, Duration::from_nanos(remaining), selected));
    if (selected.failed) {
      return fail(make_status(StatusCode::kUnavailable,
                              "flush_out: the socket reported an error while waiting to write"));
    }
  }
}

void FrameChannel::close() noexcept {
  closed_ = true;
  (void)socket_.close();
}

Status FrameChannel::fail(Status status) {
  if (!failure_.ok()) {
    return failure_;
  }
  failure_ = std::move(status);
  (void)socket_.close();
  return failure_;
}

// ---------------------------------------------------------------------------
// INodeTransport
// ---------------------------------------------------------------------------

INodeTransport::~INodeTransport() = default;

}  // namespace rollout_fabric
