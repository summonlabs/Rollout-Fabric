// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/evidence.hpp"

#include <string>

#include "decode_util.hpp"

namespace rollout_fabric {
namespace {

using detail::dc_digest;
using detail::dc_id;
using detail::dc_string;
using detail::dc_u32;
using detail::dc_u8;
using detail::wr_digest;
using detail::wr_id;

void encode_metric(ByteWriter& writer, const Metric& metric) {
  writer.string(metric.name);
  writer.i64(metric.value_milli);
  writer.string(metric.unit);
}

[[nodiscard]] Status decode_metric(ByteReader& reader, const RuntimeLimits& limits, Metric& metric) {
  RF_TRY(dc_string(reader, "metric.name", limits.max_metric_name_bytes, metric.name));
  RF_TRY(detail::dc_i64(reader, "metric.value_milli", metric.value_milli));
  RF_TRY(dc_string(reader, "metric.unit", limits.max_metric_text_bytes, metric.unit));
  return Status::success();
}

}  // namespace

EvidenceClass evidence_class(EvidenceKind kind) noexcept {
  switch (kind) {
    case EvidenceKind::kDispatchAccepted: return EvidenceClass::kAcceptance;
    case EvidenceKind::kExecutionProgress: return EvidenceClass::kExecution;
    case EvidenceKind::kExecutionCompleted: return EvidenceClass::kExecution;
    case EvidenceKind::kExecutionFailed: return EvidenceClass::kExecution;
    case EvidenceKind::kHealthSample: return EvidenceClass::kTelemetry;
    case EvidenceKind::kRollbackCompleted: return EvidenceClass::kCompensation;
    case EvidenceKind::kRollbackFailed: return EvidenceClass::kCompensation;
    case EvidenceKind::kVerificationPassed: return EvidenceClass::kVerification;
    case EvidenceKind::kVerificationFailed: return EvidenceClass::kVerification;
    case EvidenceKind::kCancellationAcknowledged: return EvidenceClass::kCancellation;
  }
  return EvidenceClass::kAcceptance;
}

std::string_view to_string(EvidenceKind kind) noexcept {
  switch (kind) {
    case EvidenceKind::kDispatchAccepted: return "dispatch_accepted";
    case EvidenceKind::kExecutionProgress: return "execution_progress";
    case EvidenceKind::kExecutionCompleted: return "execution_completed";
    case EvidenceKind::kExecutionFailed: return "execution_failed";
    case EvidenceKind::kHealthSample: return "health_sample";
    case EvidenceKind::kRollbackCompleted: return "rollback_completed";
    case EvidenceKind::kRollbackFailed: return "rollback_failed";
    case EvidenceKind::kVerificationPassed: return "verification_passed";
    case EvidenceKind::kVerificationFailed: return "verification_failed";
    case EvidenceKind::kCancellationAcknowledged: return "cancellation_acknowledged";
  }
  return "unknown";
}

std::optional<EvidenceKind> parse_evidence_kind(std::string_view text) noexcept {
  if (text == "dispatch_accepted") return EvidenceKind::kDispatchAccepted;
  if (text == "execution_progress") return EvidenceKind::kExecutionProgress;
  if (text == "execution_completed") return EvidenceKind::kExecutionCompleted;
  if (text == "execution_failed") return EvidenceKind::kExecutionFailed;
  if (text == "health_sample") return EvidenceKind::kHealthSample;
  if (text == "rollback_completed") return EvidenceKind::kRollbackCompleted;
  if (text == "rollback_failed") return EvidenceKind::kRollbackFailed;
  if (text == "verification_passed") return EvidenceKind::kVerificationPassed;
  if (text == "verification_failed") return EvidenceKind::kVerificationFailed;
  if (text == "cancellation_acknowledged") return EvidenceKind::kCancellationAcknowledged;
  return std::nullopt;
}

std::string_view to_string(EvidenceClass klass) noexcept {
  switch (klass) {
    case EvidenceClass::kAcceptance: return "acceptance";
    case EvidenceClass::kExecution: return "execution";
    case EvidenceClass::kTelemetry: return "telemetry";
    case EvidenceClass::kCompensation: return "compensation";
    case EvidenceClass::kVerification: return "verification";
    case EvidenceClass::kCancellation: return "cancellation";
  }
  return "unknown";
}

std::string_view to_string(EvidenceOutcome outcome) noexcept {
  switch (outcome) {
    case EvidenceOutcome::kSucceeded: return "succeeded";
    case EvidenceOutcome::kFailed: return "failed";
    case EvidenceOutcome::kInconclusive: return "inconclusive";
  }
  return "unknown";
}

std::string_view to_string(EvidenceAuthority authority) noexcept {
  switch (authority) {
    case EvidenceAuthority::kWorkerSelfReported: return "worker_self_reported";
    case EvidenceAuthority::kExecutionAdapterAttested: return "execution_adapter_attested";
    case EvidenceAuthority::kVerifierAttested: return "verifier_attested";
    case EvidenceAuthority::kHealthSourceAttested: return "health_source_attested";
    case EvidenceAuthority::kOperatorAttested: return "operator_attested";
  }
  return "unknown";
}


std::optional<EvidenceAuthority> parse_evidence_authority(std::string_view text) noexcept {
  if (text == "worker_self_reported") return EvidenceAuthority::kWorkerSelfReported;
  if (text == "execution_adapter_attested") return EvidenceAuthority::kExecutionAdapterAttested;
  if (text == "verifier_attested") return EvidenceAuthority::kVerifierAttested;
  if (text == "health_source_attested") return EvidenceAuthority::kHealthSourceAttested;
  if (text == "operator_attested") return EvidenceAuthority::kOperatorAttested;
  return std::nullopt;
}std::string_view to_string(EvidenceOrigin origin) noexcept {
  switch (origin) {
    case EvidenceOrigin::kLive: return "live";
    case EvidenceOrigin::kRecoveredFromJournal: return "recovered_from_journal";
    case EvidenceOrigin::kReconciledFromAdapter: return "reconciled_from_adapter";
  }
  return "unknown";
}

std::string_view to_string(EvidenceAdmission admission) noexcept {
  switch (admission) {
    case EvidenceAdmission::kAccepted: return "accepted";
    case EvidenceAdmission::kDuplicate: return "duplicate";
    case EvidenceAdmission::kOutOfOrder: return "out_of_order";
    case EvidenceAdmission::kStaleAttempt: return "stale_attempt";
    case EvidenceAdmission::kStaleGeneration: return "stale_generation";
    case EvidenceAdmission::kStaleIncarnation: return "stale_incarnation";
    case EvidenceAdmission::kUnknownAttempt: return "unknown_attempt";
    case EvidenceAdmission::kUntrustedAuthority: return "untrusted_authority";
    case EvidenceAdmission::kExpired: return "expired";
    case EvidenceAdmission::kClockAnomaly: return "clock_anomaly";
    case EvidenceAdmission::kMalformed: return "malformed";
    case EvidenceAdmission::kBackpressure: return "backpressure";
  }
  return "unknown";
}

Digest256 EvidenceRecord::record_digest() const {
  ByteWriter writer;
  writer.string("rollout-fabric/evidence/v1");
  wr_id(writer, id);
  wr_id(writer, rollout);
  wr_id(writer, generation);
  wr_id(writer, stage);
  wr_id(writer, cohort);
  wr_id(writer, target);
  wr_id(writer, attempt);
  writer.u64(value_of(attempt_epoch));
  wr_id(writer, incarnation);
  writer.u8(static_cast<std::uint8_t>(kind));
  writer.u8(static_cast<std::uint8_t>(outcome));
  writer.u8(static_cast<std::uint8_t>(authority));
  writer.u8(static_cast<std::uint8_t>(origin));
  writer.u64(value_of(sequence));
  writer.i64(observed_at.unix_nanos());
  writer.i64(recorded_at.unix_nanos());
  writer.i64(ttl.nanos());
  wr_id(writer, health_source);
  wr_id(writer, verifier);
  wr_digest(writer, payload_digest);
  writer.u32(static_cast<std::uint32_t>(metrics.size()));
  for (const Metric& metric : metrics) {
    encode_metric(writer, metric);
  }
  return sha256(writer.span());
}

Status EvidenceRecord::validate(const RuntimeLimits& limits) const {
  if (id.is_nil()) {
    return make_status(StatusCode::kInvalidArgument, "evidence identifier is nil");
  }
  // Telemetry is a statement about a target, not about an attempt: a health
  // source observes infrastructure, it does not participate in a rollout. Every
  // other class of evidence is anchored to the attempt it describes.
  const bool telemetry = evidence_class(kind) == EvidenceClass::kTelemetry;
  if (!telemetry &&
      (rollout.is_nil() || generation.is_nil() || stage.is_nil() || attempt.is_nil())) {
    return make_status(StatusCode::kInvalidArgument,
                       "evidence must name the rollout, generation, stage and attempt it belongs to");
  }
  if (target.is_nil()) {
    return make_status(StatusCode::kInvalidArgument, "evidence must name its target");
  }
  if (metrics.size() > limits.max_metrics_per_evidence) {
    return make_status(StatusCode::kLimitExceeded,
                       "evidence carries " + std::to_string(metrics.size()) +
                           " metrics, above max_metrics_per_evidence");
  }
  for (const Metric& metric : metrics) {
    if (metric.name.empty() || metric.name.size() > limits.max_metric_name_bytes) {
      return make_status(StatusCode::kInvalidArgument, "metric name is empty or too long");
    }
    if (metric.unit.size() > limits.max_metric_text_bytes) {
      return make_status(StatusCode::kInvalidArgument, "metric unit is too long");
    }
  }
  if (ttl.is_negative()) {
    return make_status(StatusCode::kInvalidArgument, "evidence ttl must not be negative");
  }
  if (evidence_class(kind) == EvidenceClass::kTelemetry && health_source.is_nil()) {
    return make_status(StatusCode::kInvalidArgument,
                       "a health sample must name the health source that attested it");
  }
  if (evidence_class(kind) == EvidenceClass::kVerification && verifier.is_nil()) {
    return make_status(StatusCode::kInvalidArgument,
                       "a verification result must name the verifier that produced it");
  }
  return Status::success();
}

void encode(ByteWriter& writer, const EvidenceRecord& value) {
  wr_id(writer, value.id);
  wr_id(writer, value.rollout);
  wr_id(writer, value.generation);
  wr_id(writer, value.stage);
  wr_id(writer, value.cohort);
  wr_id(writer, value.target);
  wr_id(writer, value.attempt);
  writer.u64(value_of(value.attempt_epoch));
  wr_id(writer, value.incarnation);
  writer.u8(static_cast<std::uint8_t>(value.kind));
  writer.u8(static_cast<std::uint8_t>(value.outcome));
  writer.u8(static_cast<std::uint8_t>(value.authority));
  writer.u8(static_cast<std::uint8_t>(value.origin));
  writer.u64(value_of(value.sequence));
  writer.i64(value.observed_at.unix_nanos());
  writer.i64(value.recorded_at.unix_nanos());
  writer.i64(value.ttl.nanos());
  wr_id(writer, value.health_source);
  wr_id(writer, value.verifier);
  wr_digest(writer, value.payload_digest);
  writer.u32(static_cast<std::uint32_t>(value.metrics.size()));
  for (const Metric& metric : value.metrics) {
    encode_metric(writer, metric);
  }
}

Result<EvidenceRecord> decode_evidence_record(ByteReader& reader, const RuntimeLimits& limits) {
  EvidenceRecord value;
  RF_TRY(dc_id(reader, "evidence.id", value.id));
  RF_TRY(dc_id(reader, "evidence.rollout", value.rollout));
  RF_TRY(dc_id(reader, "evidence.generation", value.generation));
  RF_TRY(dc_id(reader, "evidence.stage", value.stage));
  RF_TRY(dc_id(reader, "evidence.cohort", value.cohort));
  RF_TRY(dc_id(reader, "evidence.target", value.target));
  RF_TRY(dc_id(reader, "evidence.attempt", value.attempt));
  std::uint64_t epoch = 0;
  RF_TRY(detail::dc_u64(reader, "evidence.attempt_epoch", epoch));
  value.attempt_epoch = AttemptEpoch{epoch};
  RF_TRY(dc_id(reader, "evidence.incarnation", value.incarnation));
  std::uint8_t kind = 0;
  RF_TRY(dc_u8(reader, "evidence.kind", kind));
  if (kind > static_cast<std::uint8_t>(EvidenceKind::kCancellationAcknowledged)) {
    return make_status(StatusCode::kCorrupt, "unknown evidence kind " + std::to_string(kind));
  }
  value.kind = static_cast<EvidenceKind>(kind);
  std::uint8_t outcome = 0;
  RF_TRY(dc_u8(reader, "evidence.outcome", outcome));
  if (outcome > static_cast<std::uint8_t>(EvidenceOutcome::kInconclusive)) {
    return make_status(StatusCode::kCorrupt, "unknown evidence outcome");
  }
  value.outcome = static_cast<EvidenceOutcome>(outcome);
  std::uint8_t authority = 0;
  RF_TRY(dc_u8(reader, "evidence.authority", authority));
  if (authority > static_cast<std::uint8_t>(EvidenceAuthority::kOperatorAttested)) {
    return make_status(StatusCode::kCorrupt, "unknown evidence authority");
  }
  value.authority = static_cast<EvidenceAuthority>(authority);
  std::uint8_t origin = 0;
  RF_TRY(dc_u8(reader, "evidence.origin", origin));
  if (origin > static_cast<std::uint8_t>(EvidenceOrigin::kReconciledFromAdapter)) {
    return make_status(StatusCode::kCorrupt, "unknown evidence origin");
  }
  value.origin = static_cast<EvidenceOrigin>(origin);
  std::uint64_t sequence = 0;
  RF_TRY(detail::dc_u64(reader, "evidence.sequence", sequence));
  value.sequence = Sequence{sequence};
  RF_TRY(detail::dc_timestamp(reader, "evidence.observed_at", value.observed_at));
  RF_TRY(detail::dc_timestamp(reader, "evidence.recorded_at", value.recorded_at));
  RF_TRY(detail::dc_duration(reader, "evidence.ttl", value.ttl));
  RF_TRY(dc_id(reader, "evidence.health_source", value.health_source));
  RF_TRY(dc_id(reader, "evidence.verifier", value.verifier));
  RF_TRY(dc_digest(reader, "evidence.payload_digest", value.payload_digest));
  std::uint32_t metric_count = 0;
  RF_TRY(dc_u32(reader, "evidence.metrics", metric_count));
  if (metric_count > limits.max_metrics_per_evidence) {
    return make_status(StatusCode::kLimitExceeded, "evidence metric count exceeds the bound");
  }
  value.metrics.reserve(metric_count);
  for (std::uint32_t index = 0; index < metric_count; ++index) {
    Metric metric;
    RF_TRY(decode_metric(reader, limits, metric));
    value.metrics.push_back(std::move(metric));
  }
  const Status status = value.validate(limits);
  if (!status.ok()) {
    return status;
  }
  return value;
}

}  // namespace rollout_fabric
