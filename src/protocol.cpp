// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "decode_util.hpp"

namespace rollout_fabric {
namespace {

// Copies a finished body out of the writer. ByteWriter owns its buffer, so the
// encoding step hands the caller its own copy.
[[nodiscard]] std::vector<std::byte> finish(ByteWriter& writer) {
  return std::vector<std::byte>(writer.data().begin(), writer.data().end());
}

[[nodiscard]] Status unknown_value(const char* field, std::uint64_t raw) {
  return make_status(StatusCode::kCorrupt,
                     std::string(field) + ": unknown value " + std::to_string(raw));
}

[[nodiscard]] Status over_ceiling(const char* field, std::uint64_t declared, std::uint64_t maximum) {
  return make_status(StatusCode::kLimitExceeded,
                     std::string(field) + ": declared count " + std::to_string(declared) +
                         " exceeds the maximum " + std::to_string(maximum));
}

template <class Enum>
[[nodiscard]] Status decode_enum(ByteReader& reader, const char* field, std::uint8_t maximum,
                                 Enum& out) {
  std::uint8_t raw = 0;
  RF_TRY(detail::dc_u8(reader, field, raw));
  if (raw > maximum) {
    return unknown_value(field, raw);
  }
  out = static_cast<Enum>(raw);
  return Status::success();
}

[[nodiscard]] Status decode_status_code(ByteReader& reader, const char* field, StatusCode& out) {
  std::uint16_t raw = 0;
  RF_TRY(detail::dc_u16(reader, field, raw));
  if (raw > static_cast<std::uint16_t>(StatusCode::kUnsupportedGate)) {
    return unknown_value(field, raw);
  }
  out = static_cast<StatusCode>(raw);
  return Status::success();
}

void write_fence(ByteWriter& writer, const AttemptFence& fence) {
  detail::wr_id(writer, fence.rollout);
  detail::wr_id(writer, fence.generation);
  detail::wr_id(writer, fence.stage);
  detail::wr_id(writer, fence.attempt);
  writer.u64(value_of(fence.epoch));
  detail::wr_id(writer, fence.incarnation);
}

[[nodiscard]] Status read_fence(ByteReader& reader, const char* field, AttemptFence& out) {
  RF_TRY(detail::dc_id(reader, field, out.rollout));
  RF_TRY(detail::dc_id(reader, field, out.generation));
  RF_TRY(detail::dc_id(reader, field, out.stage));
  RF_TRY(detail::dc_id(reader, field, out.attempt));
  std::uint64_t epoch = 0;
  RF_TRY(detail::dc_u64(reader, field, epoch));
  out.epoch = AttemptEpoch{epoch};
  RF_TRY(detail::dc_id(reader, field, out.incarnation));
  return Status::success();
}

}  // namespace

// ---------------------------------------------------------------------------
// Hello
// ---------------------------------------------------------------------------

std::vector<std::byte> encode(const HelloMessage& message) {
  ByteWriter writer;
  writer.u32(message.protocol_version);
  writer.u32(message.abi_version);
  detail::wr_id(writer, message.worker);
  detail::wr_id(writer, message.incarnation);
  writer.string(message.name);
  writer.u32(message.capacity);
  writer.u64(message.started_at_unix_nanos);
  return finish(writer);
}

Result<HelloMessage> decode_hello(std::span<const std::byte> body, const RuntimeLimits& limits) {
  ByteReader reader(body);
  HelloMessage message;
  RF_TRY(detail::dc_u32(reader, "protocol_version", message.protocol_version));
  if (message.protocol_version != kProtocolVersion) {
    return make_status(StatusCode::kCorrupt,
                       "protocol_version " + std::to_string(message.protocol_version) +
                           " is not one this build speaks (it speaks " +
                           std::to_string(kProtocolVersion) + ")");
  }
  RF_TRY(detail::dc_u32(reader, "abi_version", message.abi_version));
  RF_TRY(detail::dc_id(reader, "worker", message.worker));
  RF_TRY(detail::dc_id(reader, "incarnation", message.incarnation));
  RF_TRY(detail::dc_string(reader, "name", limits.max_name_bytes, message.name));
  RF_TRY(detail::dc_u32(reader, "capacity", message.capacity));
  if (message.capacity > limits.max_concurrent_attempts) {
    return make_status(StatusCode::kLimitExceeded,
                       "capacity " + std::to_string(message.capacity) +
                           " exceeds the configured ceiling of " +
                           std::to_string(limits.max_concurrent_attempts));
  }
  RF_TRY(detail::dc_u64(reader, "started_at_unix_nanos", message.started_at_unix_nanos));
  RF_TRY(reader.expect_end());
  return message;
}

std::vector<std::byte> encode(const HelloAckMessage& message) {
  ByteWriter writer;
  writer.u32(message.protocol_version);
  writer.u32(message.abi_version);
  detail::wr_id(writer, message.controller_incarnation);
  writer.u64(value_of(message.controller_epoch));
  writer.u32(message.max_in_flight);
  return finish(writer);
}

Result<HelloAckMessage> decode_hello_ack(std::span<const std::byte> body,
                                         const RuntimeLimits& limits) {
  (void)limits;
  ByteReader reader(body);
  HelloAckMessage message;
  RF_TRY(detail::dc_u32(reader, "protocol_version", message.protocol_version));
  RF_TRY(detail::dc_u32(reader, "abi_version", message.abi_version));
  RF_TRY(detail::dc_id(reader, "controller_incarnation", message.controller_incarnation));
  std::uint64_t epoch = 0;
  RF_TRY(detail::dc_u64(reader, "controller_epoch", epoch));
  message.controller_epoch = EpochCounter{epoch};
  RF_TRY(detail::dc_u32(reader, "max_in_flight", message.max_in_flight));
  RF_TRY(reader.expect_end());
  return message;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

std::vector<std::byte> encode(const DispatchCommandMessage& message, const RuntimeLimits& limits) {
  // Encoding never drops a field: the parameter block is carried verbatim and
  // the ceiling is enforced by decode_dispatch, which can report a failure.
  (void)limits;
  ByteWriter writer;
  write_fence(writer, message.request.fence);
  detail::wr_id(writer, message.request.cohort);
  detail::wr_id(writer, message.request.action);
  writer.raw(message.request.action_digest);
  writer.raw(message.request.artifact_digest);
  writer.raw(message.request.idempotency_key);
  writer.u32(message.request.attempt_number);
  writer.bytes(message.request.parameters);
  detail::wr_timestamp(writer, message.request.deadline);
  detail::wr_id(writer, message.target);
  writer.string(message.target_name);
  return finish(writer);
}

Result<DispatchCommandMessage> decode_dispatch(std::span<const std::byte> body,
                                        const RuntimeLimits& limits) {
  ByteReader reader(body);
  DispatchCommandMessage message;
  RF_TRY(read_fence(reader, "fence", message.request.fence));
  RF_TRY(detail::dc_id(reader, "cohort", message.request.cohort));
  RF_TRY(detail::dc_id(reader, "action", message.request.action));
  RF_TRY(detail::dc_digest(reader, "action_digest", message.request.action_digest));
  RF_TRY(detail::dc_digest(reader, "artifact_digest", message.request.artifact_digest));
  RF_TRY(detail::dc_digest(reader, "idempotency_key", message.request.idempotency_key));
  RF_TRY(detail::dc_u32(reader, "attempt_number", message.request.attempt_number));
  RF_TRY(detail::dc_bytes(reader, "parameters", limits.max_message_bytes,
                          message.request.parameters));
  RF_TRY(detail::dc_timestamp(reader, "deadline", message.request.deadline));
  RF_TRY(detail::dc_id(reader, "target", message.target));
  RF_TRY(detail::dc_string(reader, "target_name", limits.max_name_bytes, message.target_name));
  RF_TRY(reader.expect_end());
  return message;
}

std::vector<std::byte> encode(const DispatchAckMessage& message) {
  ByteWriter writer;
  write_fence(writer, message.fence);
  writer.u8(static_cast<std::uint8_t>(message.outcome));
  writer.string(message.detail);
  detail::wr_timestamp(writer, message.accepted_at);
  return finish(writer);
}

Result<DispatchAckMessage> decode_dispatch_ack(std::span<const std::byte> body,
                                               const RuntimeLimits& limits) {
  ByteReader reader(body);
  DispatchAckMessage message;
  RF_TRY(read_fence(reader, "fence", message.fence));
  RF_TRY(decode_enum(reader, "outcome", static_cast<std::uint8_t>(DispatchOutcome::kFenced),
                     message.outcome));
  RF_TRY(detail::dc_string(reader, "detail", limits.max_description_bytes, message.detail));
  RF_TRY(detail::dc_timestamp(reader, "accepted_at", message.accepted_at));
  RF_TRY(reader.expect_end());
  return message;
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

std::vector<std::byte> encode(const CancelMessage& message) {
  ByteWriter writer;
  write_fence(writer, message.fence);
  writer.string(message.reason);
  return finish(writer);
}

Result<CancelMessage> decode_cancel(std::span<const std::byte> body, const RuntimeLimits& limits) {
  ByteReader reader(body);
  CancelMessage message;
  RF_TRY(read_fence(reader, "fence", message.fence));
  RF_TRY(detail::dc_string(reader, "reason", limits.max_description_bytes, message.reason));
  RF_TRY(reader.expect_end());
  return message;
}

std::vector<std::byte> encode(const CancelAckMessage& message) {
  ByteWriter writer;
  write_fence(writer, message.fence);
  writer.boolean(message.stopped);
  writer.string(message.detail);
  return finish(writer);
}

Result<CancelAckMessage> decode_cancel_ack(std::span<const std::byte> body,
                                           const RuntimeLimits& limits) {
  ByteReader reader(body);
  CancelAckMessage message;
  RF_TRY(read_fence(reader, "fence", message.fence));
  RF_TRY(detail::dc_bool(reader, "stopped", message.stopped));
  RF_TRY(detail::dc_string(reader, "detail", limits.max_description_bytes, message.detail));
  RF_TRY(reader.expect_end());
  return message;
}

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------

std::vector<std::byte> encode(const EvidenceMessage& message, const RuntimeLimits& limits) {
  // The record codec owns its own bounds; nothing is added or dropped here.
  (void)limits;
  ByteWriter writer;
  encode(writer, message.record);
  return finish(writer);
}

Result<EvidenceMessage> decode_evidence(std::span<const std::byte> body,
                                        const RuntimeLimits& limits) {
  ByteReader reader(body);
  EvidenceMessage message;
  RF_TRY_ASSIGN(decode_evidence_record(reader, limits), message.record);
  RF_TRY(reader.expect_end());
  return message;
}

// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

std::vector<std::byte> encode(const HeartbeatMessage& message) {
  ByteWriter writer;
  detail::wr_id(writer, message.incarnation);
  writer.u32(message.in_flight);
  writer.u64(message.completed);
  writer.u64(message.failed);
  detail::wr_timestamp(writer, message.at);
  return finish(writer);
}

Result<HeartbeatMessage> decode_heartbeat(std::span<const std::byte> body,
                                          const RuntimeLimits& limits) {
  (void)limits;
  ByteReader reader(body);
  HeartbeatMessage message;
  RF_TRY(detail::dc_id(reader, "incarnation", message.incarnation));
  RF_TRY(detail::dc_u32(reader, "in_flight", message.in_flight));
  RF_TRY(detail::dc_u64(reader, "completed", message.completed));
  RF_TRY(detail::dc_u64(reader, "failed", message.failed));
  RF_TRY(detail::dc_timestamp(reader, "at", message.at));
  RF_TRY(reader.expect_end());
  return message;
}

std::vector<std::byte> encode(const HeartbeatAckMessage& message) {
  ByteWriter writer;
  detail::wr_id(writer, message.controller_incarnation);
  writer.u64(value_of(message.controller_epoch));
  writer.boolean(message.accept_work);
  return finish(writer);
}

Result<HeartbeatAckMessage> decode_heartbeat_ack(std::span<const std::byte> body,
                                                 const RuntimeLimits& limits) {
  (void)limits;
  ByteReader reader(body);
  HeartbeatAckMessage message;
  RF_TRY(detail::dc_id(reader, "controller_incarnation", message.controller_incarnation));
  std::uint64_t epoch = 0;
  RF_TRY(detail::dc_u64(reader, "controller_epoch", epoch));
  message.controller_epoch = EpochCounter{epoch};
  RF_TRY(detail::dc_bool(reader, "accept_work", message.accept_work));
  RF_TRY(reader.expect_end());
  return message;
}

// ---------------------------------------------------------------------------
// Reconcile
// ---------------------------------------------------------------------------

std::vector<std::byte> encode(const ReconcileRequestMessage& message) {
  ByteWriter writer;
  writer.u64(message.request_id);
  writer.u32(static_cast<std::uint32_t>(message.fences.size()));
  for (const AttemptFence& fence : message.fences) {
    write_fence(writer, fence);
  }
  return finish(writer);
}

Result<ReconcileRequestMessage> decode_reconcile_request(std::span<const std::byte> body,
                                                         const RuntimeLimits& limits) {
  ByteReader reader(body);
  ReconcileRequestMessage message;
  RF_TRY(detail::dc_u64(reader, "request_id", message.request_id));
  std::uint32_t count = 0;
  RF_TRY(detail::dc_u32(reader, "fences", count));
  if (count > limits.max_concurrent_attempts) {
    return over_ceiling("fences", count, limits.max_concurrent_attempts);
  }
  message.fences.clear();
  message.fences.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    AttemptFence fence;
    RF_TRY(read_fence(reader, "fences", fence));
    message.fences.push_back(fence);
  }
  RF_TRY(reader.expect_end());
  return message;
}

std::vector<std::byte> encode(const ReconcileResponseMessage& message) {
  ByteWriter writer;
  writer.u64(message.request_id);
  writer.u32(static_cast<std::uint32_t>(message.entries.size()));
  for (const ReconcileEntry& entry : message.entries) {
    write_fence(writer, entry.fence);
    writer.u8(static_cast<std::uint8_t>(entry.disposition));
    writer.u8(static_cast<std::uint8_t>(entry.outcome));
    writer.string(entry.detail);
  }
  return finish(writer);
}

Result<ReconcileResponseMessage> decode_reconcile_response(std::span<const std::byte> body,
                                                           const RuntimeLimits& limits) {
  ByteReader reader(body);
  ReconcileResponseMessage message;
  RF_TRY(detail::dc_u64(reader, "request_id", message.request_id));
  std::uint32_t count = 0;
  RF_TRY(detail::dc_u32(reader, "entries", count));
  if (count > limits.max_concurrent_attempts) {
    return over_ceiling("entries", count, limits.max_concurrent_attempts);
  }
  message.entries.clear();
  message.entries.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    ReconcileEntry entry;
    RF_TRY(read_fence(reader, "entries", entry.fence));
    RF_TRY(decode_enum(reader, "disposition",
                       static_cast<std::uint8_t>(ReconcileDisposition::kUnknown),
                       entry.disposition));
    RF_TRY(decode_enum(reader, "outcome", static_cast<std::uint8_t>(EvidenceOutcome::kInconclusive),
                       entry.outcome));
    RF_TRY(detail::dc_string(reader, "detail", limits.max_description_bytes, entry.detail));
    message.entries.push_back(std::move(entry));
  }
  RF_TRY(reader.expect_end());
  return message;
}

// ---------------------------------------------------------------------------
// Shutdown and error
// ---------------------------------------------------------------------------

std::vector<std::byte> encode(const ShutdownMessage& message) {
  ByteWriter writer;
  writer.string(message.reason);
  return finish(writer);
}

Result<ShutdownMessage> decode_shutdown(std::span<const std::byte> body,
                                        const RuntimeLimits& limits) {
  ByteReader reader(body);
  ShutdownMessage message;
  RF_TRY(detail::dc_string(reader, "reason", limits.max_description_bytes, message.reason));
  RF_TRY(reader.expect_end());
  return message;
}

std::vector<std::byte> encode(const ErrorMessage& message) {
  ByteWriter writer;
  writer.u16(static_cast<std::uint16_t>(message.code));
  writer.string(message.detail);
  return finish(writer);
}

Result<ErrorMessage> decode_error(std::span<const std::byte> body, const RuntimeLimits& limits) {
  ByteReader reader(body);
  ErrorMessage message;
  RF_TRY(decode_status_code(reader, "code", message.code));
  RF_TRY(detail::dc_string(reader, "detail", limits.max_description_bytes, message.detail));
  RF_TRY(reader.expect_end());
  return message;
}

// ---------------------------------------------------------------------------
// Operator commands
// ---------------------------------------------------------------------------

std::vector<std::byte> encode(const CommandRequestMessage& message) {
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(message.kind));
  detail::wr_id(writer, message.rollout);
  detail::wr_id(writer, message.gate);
  detail::wr_id(writer, message.generation);
  writer.u64(value_of(message.revision));
  writer.boolean(message.checked_generation);
  writer.boolean(message.checked_revision);
  writer.string(message.reason);
  writer.string(message.operator_name);
  writer.string(message.plan_path);
  return finish(writer);
}

Result<CommandRequestMessage> decode_command_request(std::span<const std::byte> body,
                                                     const RuntimeLimits& limits) {
  ByteReader reader(body);
  CommandRequestMessage message;
  RF_TRY(decode_enum(reader, "kind", static_cast<std::uint8_t>(CommandKind::kListEvents), message.kind));
  RF_TRY(detail::dc_id(reader, "rollout", message.rollout));
  RF_TRY(detail::dc_id(reader, "gate", message.gate));
  RF_TRY(detail::dc_id(reader, "generation", message.generation));
  std::uint64_t revision = 0;
  RF_TRY(detail::dc_u64(reader, "revision", revision));
  message.revision = Revision{revision};
  RF_TRY(detail::dc_bool(reader, "checked_generation", message.checked_generation));
  RF_TRY(detail::dc_bool(reader, "checked_revision", message.checked_revision));
  RF_TRY(detail::dc_string(reader, "reason", limits.max_description_bytes, message.reason));
  RF_TRY(detail::dc_string(reader, "operator_name", limits.max_name_bytes, message.operator_name));
  RF_TRY(detail::dc_string(reader, "plan_path", limits.max_description_bytes, message.plan_path));
  RF_TRY(reader.expect_end());
  return message;
}

std::vector<std::byte> encode(const CommandResponseMessage& message) {
  ByteWriter writer;
  writer.u16(static_cast<std::uint16_t>(message.code));
  writer.string(message.detail);
  writer.string(message.payload);
  detail::wr_id(writer, message.rollout);
  detail::wr_id(writer, message.gate);
  return finish(writer);
}

Result<CommandResponseMessage> decode_command_response(std::span<const std::byte> body,
                                                       const RuntimeLimits& limits) {
  ByteReader reader(body);
  CommandResponseMessage message;
  RF_TRY(decode_status_code(reader, "code", message.code));
  RF_TRY(detail::dc_string(reader, "detail", limits.max_description_bytes, message.detail));
  RF_TRY(detail::dc_string(reader, "payload", limits.max_message_bytes, message.payload));
  RF_TRY(detail::dc_id(reader, "rollout", message.rollout));
  RF_TRY(detail::dc_id(reader, "gate", message.gate));
  RF_TRY(reader.expect_end());
  return message;
}

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------

std::vector<std::byte> encode(const HealthRequestMessage& message) {
  ByteWriter writer;
  writer.u64(message.request_id);
  writer.u32(static_cast<std::uint32_t>(message.targets.size()));
  for (const TargetId& target : message.targets) {
    detail::wr_id(writer, target);
  }
  detail::wr_timestamp(writer, message.issued_at);
  return finish(writer);
}

Result<HealthRequestMessage> decode_health_request(std::span<const std::byte> body,
                                                   const RuntimeLimits& limits) {
  ByteReader reader(body);
  HealthRequestMessage message;
  RF_TRY(detail::dc_u64(reader, "request_id", message.request_id));
  RF_TRY(detail::dc_id_vector(reader, "targets", limits.max_total_targets, message.targets));
  RF_TRY(detail::dc_timestamp(reader, "issued_at", message.issued_at));
  RF_TRY(reader.expect_end());
  return message;
}

std::vector<std::byte> encode(const HealthResponseMessage& message, const RuntimeLimits& limits) {
  // The sample count is bounded by decode_health_response, which can report it.
  (void)limits;
  ByteWriter writer;
  writer.u64(message.request_id);
  detail::wr_id(writer, message.source);
  detail::wr_timestamp(writer, message.observed_at);
  writer.boolean(message.live);
  writer.u32(static_cast<std::uint32_t>(message.samples.size()));
  for (const HealthSample& sample : message.samples) {
    detail::wr_id(writer, sample.target);
    writer.boolean(sample.healthy);
    detail::wr_timestamp(writer, sample.observed_at);
    writer.string(sample.reason);
  }
  return finish(writer);
}

Result<HealthResponseMessage> decode_health_response(std::span<const std::byte> body,
                                                     const RuntimeLimits& limits) {
  ByteReader reader(body);
  HealthResponseMessage message;
  RF_TRY(detail::dc_u64(reader, "request_id", message.request_id));
  RF_TRY(detail::dc_id(reader, "source", message.source));
  RF_TRY(detail::dc_timestamp(reader, "observed_at", message.observed_at));
  RF_TRY(detail::dc_bool(reader, "live", message.live));
  std::uint32_t count = 0;
  RF_TRY(detail::dc_u32(reader, "samples", count));
  if (count > limits.max_total_targets) {
    return over_ceiling("samples", count, limits.max_total_targets);
  }
  message.samples.clear();
  message.samples.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    HealthSample sample;
    RF_TRY(detail::dc_id(reader, "samples", sample.target));
    RF_TRY(detail::dc_bool(reader, "healthy", sample.healthy));
    RF_TRY(detail::dc_timestamp(reader, "observed_at", sample.observed_at));
    RF_TRY(detail::dc_string(reader, "reason", limits.max_description_bytes, sample.reason));
    message.samples.push_back(std::move(sample));
  }
  RF_TRY(reader.expect_end());
  return message;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

Frame make_frame(MessageType type, Sequence sequence, std::vector<std::byte> body) {
  Frame frame;
  frame.type = type;
  frame.flags = 0;
  frame.sequence = sequence;
  frame.body = std::move(body);
  return frame;
}

}  // namespace rollout_fabric
