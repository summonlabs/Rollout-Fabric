// Rollout Fabric - control protocol message bodies.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every message body uses the canonical codec, so a message has exactly one
// encoding and its digest is meaningful. Decoders are bounded by the same
// RuntimeLimits that bound the transport, and a decoder that is handed a body
// with trailing bytes rejects it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rollout_fabric/attempt.hpp"
#include "rollout_fabric/codec.hpp"
#include "rollout_fabric/evidence.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/orchestrator.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"
#include "rollout_fabric/transport.hpp"

namespace rollout_fabric {

inline constexpr std::uint32_t kProtocolVersion = 1;

struct HelloMessage {
  std::uint32_t protocol_version = kProtocolVersion;
  std::uint32_t abi_version = 0;
  WorkerId worker{};
  IncarnationId incarnation{};
  std::string name;
  // How many attempts this worker will accept at once. The controller never
  // dispatches more than this to one worker.
  std::uint32_t capacity = 0;
  std::uint64_t started_at_unix_nanos = 0;
};

struct HelloAckMessage {
  std::uint32_t protocol_version = kProtocolVersion;
  std::uint32_t abi_version = 0;
  IncarnationId controller_incarnation{};
  EpochCounter controller_epoch{0};
  std::uint32_t max_in_flight = 0;
};

// Named DispatchCommandMessage rather than DispatchMessage because windows.h
// defines DispatchMessage as a macro aliasing DispatchMessageA, which would
// silently change the declared type in any translation unit that includes both.
struct DispatchCommandMessage {
  DispatchRequest request{};
  TargetId target{};
  std::string target_name;
};

struct DispatchAckMessage {
  AttemptFence fence{};
  DispatchOutcome outcome = DispatchOutcome::kUnavailable;
  std::string detail;
  Timestamp accepted_at{};
};

struct CancelMessage {
  AttemptFence fence{};
  std::string reason;
};

struct CancelAckMessage {
  AttemptFence fence{};
  bool stopped = false;
  std::string detail;
};

struct EvidenceMessage {
  EvidenceRecord record{};
};

struct HeartbeatMessage {
  IncarnationId incarnation{};
  std::uint32_t in_flight = 0;
  std::uint64_t completed = 0;
  std::uint64_t failed = 0;
  Timestamp at{};
};

struct HeartbeatAckMessage {
  IncarnationId controller_incarnation{};
  EpochCounter controller_epoch{0};
  bool accept_work = false;
};

struct ReconcileRequestMessage {
  std::uint64_t request_id = 0;
  std::vector<AttemptFence> fences;
};

struct ReconcileResponseMessage {
  std::uint64_t request_id = 0;
  std::vector<ReconcileEntry> entries;
};

struct ShutdownMessage {
  std::string reason;
};

struct ErrorMessage {
  StatusCode code = StatusCode::kInternal;
  std::string detail;
};

struct CommandRequestMessage {
  CommandKind kind = CommandKind::kCreate;
  RolloutId rollout{};
  // One 128-bit identifier argument whose meaning is fixed by the command kind:
  // a GateId for kApproveGate, a StageId for kInspectStage and kInspectCohort,
  // a DecisionId for kExplain. It is a tagged union of identities, not a field
  // that means different things at different times.
  GateId gate{};
  GenerationId generation{};
  Revision revision{};
  bool checked_generation = false;
  bool checked_revision = false;
  std::string reason;
  std::string operator_name;
  // Present for kCreate only.
  std::string plan_path;
};

struct CommandResponseMessage {
  StatusCode code = StatusCode::kOk;
  std::string detail;
  std::string payload;
  RolloutId rollout{};
  GateId gate{};
};

// Encoding. Each returns the canonical body of the corresponding message type.
[[nodiscard]] std::vector<std::byte> encode(const HelloMessage& message);
[[nodiscard]] Result<HelloMessage> decode_hello(std::span<const std::byte> body, const RuntimeLimits& limits);
[[nodiscard]] std::vector<std::byte> encode(const HelloAckMessage& message);
[[nodiscard]] Result<HelloAckMessage> decode_hello_ack(std::span<const std::byte> body,
                                                       const RuntimeLimits& limits);
[[nodiscard]] std::vector<std::byte> encode(const DispatchCommandMessage& message, const RuntimeLimits& limits);
[[nodiscard]] Result<DispatchCommandMessage> decode_dispatch(std::span<const std::byte> body,
                                                      const RuntimeLimits& limits);
[[nodiscard]] std::vector<std::byte> encode(const DispatchAckMessage& message);
[[nodiscard]] Result<DispatchAckMessage> decode_dispatch_ack(std::span<const std::byte> body,
                                                             const RuntimeLimits& limits);
[[nodiscard]] std::vector<std::byte> encode(const CancelMessage& message);
[[nodiscard]] Result<CancelMessage> decode_cancel(std::span<const std::byte> body, const RuntimeLimits& limits);
[[nodiscard]] std::vector<std::byte> encode(const CancelAckMessage& message);
[[nodiscard]] Result<CancelAckMessage> decode_cancel_ack(std::span<const std::byte> body,
                                                         const RuntimeLimits& limits);
[[nodiscard]] std::vector<std::byte> encode(const EvidenceMessage& message, const RuntimeLimits& limits);
[[nodiscard]] Result<EvidenceMessage> decode_evidence(std::span<const std::byte> body,
                                                      const RuntimeLimits& limits);
[[nodiscard]] std::vector<std::byte> encode(const HeartbeatMessage& message);
[[nodiscard]] Result<HeartbeatMessage> decode_heartbeat(std::span<const std::byte> body,
                                                        const RuntimeLimits& limits);
[[nodiscard]] std::vector<std::byte> encode(const HeartbeatAckMessage& message);
[[nodiscard]] Result<HeartbeatAckMessage> decode_heartbeat_ack(std::span<const std::byte> body,
                                                               const RuntimeLimits& limits);
[[nodiscard]] std::vector<std::byte> encode(const ReconcileRequestMessage& message);
[[nodiscard]] Result<ReconcileRequestMessage> decode_reconcile_request(std::span<const std::byte> body,
                                                                       const RuntimeLimits& limits);
[[nodiscard]] std::vector<std::byte> encode(const ReconcileResponseMessage& message);
[[nodiscard]] Result<ReconcileResponseMessage> decode_reconcile_response(std::span<const std::byte> body,
                                                                         const RuntimeLimits& limits);
[[nodiscard]] std::vector<std::byte> encode(const ShutdownMessage& message);
[[nodiscard]] Result<ShutdownMessage> decode_shutdown(std::span<const std::byte> body,
                                                      const RuntimeLimits& limits);
[[nodiscard]] std::vector<std::byte> encode(const ErrorMessage& message);
[[nodiscard]] Result<ErrorMessage> decode_error(std::span<const std::byte> body, const RuntimeLimits& limits);
[[nodiscard]] std::vector<std::byte> encode(const CommandRequestMessage& message);
[[nodiscard]] Result<CommandRequestMessage> decode_command_request(std::span<const std::byte> body,
                                                                   const RuntimeLimits& limits);
[[nodiscard]] std::vector<std::byte> encode(const CommandResponseMessage& message);
[[nodiscard]] Result<CommandResponseMessage> decode_command_response(std::span<const std::byte> body,
                                                                     const RuntimeLimits& limits);

struct HealthRequestMessage {
  std::uint64_t request_id = 0;
  std::vector<TargetId> targets;
  // The controller's wall clock at request time, so the worker can report an
  // observation time that is comparable.
  Timestamp issued_at{};
};

struct HealthResponseMessage {
  std::uint64_t request_id = 0;
  HealthSourceId source{};
  Timestamp observed_at{};
  bool live = false;
  std::vector<HealthSample> samples;
};

[[nodiscard]] std::vector<std::byte> encode(const HealthRequestMessage& message);
[[nodiscard]] Result<HealthRequestMessage> decode_health_request(std::span<const std::byte> body,
                                                                 const RuntimeLimits& limits);
[[nodiscard]] std::vector<std::byte> encode(const HealthResponseMessage& message, const RuntimeLimits& limits);
[[nodiscard]] Result<HealthResponseMessage> decode_health_response(std::span<const std::byte> body,
                                                                   const RuntimeLimits& limits);

// Convenience: wraps a body in a frame of the given type.
[[nodiscard]] Frame make_frame(MessageType type, Sequence sequence, std::vector<std::byte> body);

}  // namespace rollout_fabric
