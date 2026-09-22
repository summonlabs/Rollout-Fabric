// Rollout Fabric - execution and health adapter contracts.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The runtime never touches a device. It asks an execution adapter to perform
// an approved action on a target and asks a health adapter what a population
// looks like right now. Both are interfaces, so the orchestration semantics are
// exercised identically against an in-process simulator and against real
// worker processes reached over TCP - and the process-backed implementation is
// what the integration suite uses to prove the distributed claims.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rollout_fabric/attempt.hpp"
#include "rollout_fabric/digest.hpp"
#include "rollout_fabric/evidence.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"

namespace rollout_fabric {

struct DispatchRequest {
  AttemptFence fence{};
  // Which target the effect applies to. It is carried separately from the fence
  // because the fence identifies the attempt, not the thing it changes.
  TargetId target{};
  CohortId cohort{};
  ActionId action{};
  Digest256 action_digest{};
  Digest256 artifact_digest{};
  // Stable across restarts. An executor that has already performed this key
  // must report the recorded outcome instead of performing it again.
  Digest256 idempotency_key{};
  std::uint32_t attempt_number = 1;
  std::vector<std::byte> parameters;
  Timestamp deadline{};
};

enum class DispatchOutcome : std::uint8_t {
  kAccepted = 0,
  kRejected = 1,
  kUnavailable = 2,
  // The executor recognised the idempotency key and refused to repeat the
  // effect; its recorded outcome follows as evidence.
  kDuplicateSuppressed = 3,
  kThrottled = 4,
  kFenced = 5,
};

[[nodiscard]] std::string_view to_string(DispatchOutcome outcome) noexcept;

struct DispatchResult {
  DispatchOutcome outcome = DispatchOutcome::kUnavailable;
  std::string detail;
  Timestamp accepted_at{};

  [[nodiscard]] bool accepted() const noexcept {
    return outcome == DispatchOutcome::kAccepted || outcome == DispatchOutcome::kDuplicateSuppressed;
  }
};

enum class ReconcileDisposition : std::uint8_t {
  kCompleted = 0,
  kFailed = 1,
  kInProgress = 2,
  kNotStarted = 3,
  kUnknown = 4,
};

[[nodiscard]] std::string_view to_string(ReconcileDisposition disposition) noexcept;

struct ReconcileEntry {
  AttemptFence fence{};
  ReconcileDisposition disposition = ReconcileDisposition::kUnknown;
  EvidenceOutcome outcome = EvidenceOutcome::kInconclusive;
  std::string detail;
};

struct ReconcileResult {
  bool ok = false;
  std::string detail;
  std::vector<ReconcileEntry> entries;
};

struct HealthSample {
  TargetId target{};
  bool healthy = false;
  Timestamp observed_at{};
  std::string reason;
};

struct HealthProbeRequest {
  std::vector<TargetId> targets;
  Timestamp issued_at{};
  // Stable identifier so that a probe answer can be correlated with the request
  // that caused it, including across a controller restart.
  std::uint64_t request_id = 0;
};

struct HealthProbeResult {
  Status status = Status::success();
  HealthSourceId source{};
  Timestamp observed_at{};
  // A probe that was served from a cached or reconstructed view must say so.
  // The gate refuses anything that is not live.
  bool live = false;
  std::vector<HealthSample> samples;
};

// The health plane is asynchronous for the same reason the execution plane is:
// the orchestrator thread must never block on a peer. A probe is requested,
// the answer arrives as health evidence through the sink, and the aggregate
// view is available through latest() together with the time it was observed.
struct HealthSnapshotView {
  HealthSourceId source{};
  Timestamp observed_at{};
  bool live = false;
  bool available = false;
  std::vector<HealthSample> samples;
};

struct AdapterStats {
  std::uint64_t dispatched = 0;
  std::uint64_t dispatch_accepted = 0;
  std::uint64_t dispatch_refused = 0;
  std::uint64_t evidence_delivered = 0;
  std::uint64_t evidence_rejected = 0;
  std::uint64_t reconcile_requests = 0;
  std::uint64_t reconnects = 0;
  std::uint64_t protocol_errors = 0;
  std::uint64_t bytes_in = 0;
  std::uint64_t bytes_out = 0;
};

enum class AdapterEventKind : std::uint8_t {
  kWorkerConnected = 0,
  kWorkerDisconnected = 1,
  kDispatchRefused = 2,
  kProtocolError = 3,
  kWorkerIncarnationChanged = 4,
};

struct AdapterEvent {
  AdapterEventKind kind = AdapterEventKind::kWorkerConnected;
  WorkerId worker{};
  IncarnationId incarnation{};
  std::string detail;
  Timestamp at{};
};

class IEvidenceSink {
 public:
  IEvidenceSink() = default;
  virtual ~IEvidenceSink();
  IEvidenceSink(const IEvidenceSink&) = delete;
  IEvidenceSink& operator=(const IEvidenceSink&) = delete;

  virtual void on_evidence(EvidenceRecord record) = 0;
  virtual void on_adapter_event(AdapterEvent event) = 0;
};

class IExecutionAdapter {
 public:
  IExecutionAdapter() = default;
  virtual ~IExecutionAdapter();
  IExecutionAdapter(const IExecutionAdapter&) = delete;
  IExecutionAdapter& operator=(const IExecutionAdapter&) = delete;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual bool available() const noexcept = 0;
  [[nodiscard]] virtual DispatchResult dispatch(const DispatchRequest& request) = 0;
  [[nodiscard]] virtual ReconcileResult reconcile(const std::vector<AttemptFence>& attempts) = 0;
  [[nodiscard]] virtual Status cancel(const AttemptFence& fence) = 0;
  // Lets the adapter move its own bytes and hand evidence to the sink. Called
  // from the orchestrator thread only, and the adapter must not call back into
  // the orchestrator from anywhere else.
  virtual void poll(IEvidenceSink& sink) = 0;
  [[nodiscard]] virtual AdapterStats stats() const = 0;
};

class IHealthAdapter {
 public:
  IHealthAdapter() = default;
  virtual ~IHealthAdapter();
  IHealthAdapter(const IHealthAdapter&) = delete;
  IHealthAdapter& operator=(const IHealthAdapter&) = delete;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual HealthSourceId source_id() const noexcept = 0;
  [[nodiscard]] virtual bool available() const noexcept = 0;
  // Issues a probe. Non-blocking: the samples arrive as evidence through the
  // sink, carrying their own observation time and authority.
  [[nodiscard]] virtual Status request_probe(const HealthProbeRequest& request) = 0;
  // The most recent view the adapter holds, with the time it was observed. It
  // may be stale; the caller decides, using freshness rules, whether to use it.
  [[nodiscard]] virtual HealthSnapshotView latest(const std::vector<TargetId>& targets) const = 0;
  [[nodiscard]] virtual AdapterStats stats() const = 0;
};

}  // namespace rollout_fabric
