// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/attempt.hpp"

#include <string>

#include "decode_util.hpp"

namespace rollout_fabric {
namespace {

using detail::dc_digest;
using detail::dc_id;
using detail::dc_u32;
using detail::dc_u8;
using detail::wr_digest;
using detail::wr_id;

}  // namespace

Digest256 AttemptFence::fence_digest() const {
  ByteWriter writer;
  writer.string("rollout-fabric/attempt-fence/v1");
  wr_id(writer, rollout);
  wr_id(writer, generation);
  wr_id(writer, stage);
  wr_id(writer, attempt);
  writer.u64(value_of(epoch));
  wr_id(writer, incarnation);
  return sha256(writer.span());
}

Digest256 derive_idempotency_key(const RolloutId& rollout, const GenerationId& generation,
                                 const StageId& stage, const TargetId& target,
                                 const ActionId& action) noexcept {
  ByteWriter writer;
  writer.string("rollout-fabric/idempotency-key/v1");
  wr_id(writer, rollout);
  wr_id(writer, generation);
  wr_id(writer, stage);
  wr_id(writer, target);
  wr_id(writer, action);
  return sha256(writer.span());
}

void encode(ByteWriter& writer, const Attempt& value) {
  wr_id(writer, value.id);
  wr_id(writer, value.rollout);
  wr_id(writer, value.generation);
  wr_id(writer, value.stage);
  wr_id(writer, value.cohort);
  wr_id(writer, value.target);
  wr_id(writer, value.action);
  writer.u64(value_of(value.epoch));
  writer.u32(value.attempt_number);
  wr_id(writer, value.incarnation);
  wr_digest(writer, value.action_digest);
  wr_digest(writer, value.artifact_digest);
  wr_digest(writer, value.idempotency_key);
  writer.i64(value.created_at.unix_nanos());
  writer.i64(value.dispatched_at.unix_nanos());
  writer.i64(value.deadline.unix_nanos());
  writer.i64(value.last_evidence_at.unix_nanos());
  writer.u8(static_cast<std::uint8_t>(value.state));
  writer.boolean(value.completed);
  writer.u8(static_cast<std::uint8_t>(value.outcome));
  writer.u64(value_of(value.cancelled_epoch));
  writer.u64(value_of(value.highest_sequence));
  writer.u32(value.accepted_evidence);
  writer.u32(value.duplicate_evidence);
  writer.u32(value.out_of_order_evidence);
}

Result<Attempt> decode_attempt(ByteReader& reader, const RuntimeLimits& limits) {
  (void)limits;
  Attempt value;
  RF_TRY(dc_id(reader, "attempt.id", value.id));
  RF_TRY(dc_id(reader, "attempt.rollout", value.rollout));
  RF_TRY(dc_id(reader, "attempt.generation", value.generation));
  RF_TRY(dc_id(reader, "attempt.stage", value.stage));
  RF_TRY(dc_id(reader, "attempt.cohort", value.cohort));
  RF_TRY(dc_id(reader, "attempt.target", value.target));
  RF_TRY(dc_id(reader, "attempt.action", value.action));
  std::uint64_t epoch = 0;
  RF_TRY(detail::dc_u64(reader, "attempt.epoch", epoch));
  value.epoch = AttemptEpoch{epoch};
  RF_TRY(dc_u32(reader, "attempt.attempt_number", value.attempt_number));
  RF_TRY(dc_id(reader, "attempt.incarnation", value.incarnation));
  RF_TRY(dc_digest(reader, "attempt.action_digest", value.action_digest));
  RF_TRY(dc_digest(reader, "attempt.artifact_digest", value.artifact_digest));
  RF_TRY(dc_digest(reader, "attempt.idempotency_key", value.idempotency_key));
  RF_TRY(detail::dc_timestamp(reader, "attempt.created_at", value.created_at));
  RF_TRY(detail::dc_timestamp(reader, "attempt.dispatched_at", value.dispatched_at));
  RF_TRY(detail::dc_timestamp(reader, "attempt.deadline", value.deadline));
  RF_TRY(detail::dc_timestamp(reader, "attempt.last_evidence_at", value.last_evidence_at));
  std::uint8_t state = 0;
  RF_TRY(dc_u8(reader, "attempt.state", state));
  if (state > static_cast<std::uint8_t>(TargetState::kUnknown)) {
    return make_status(StatusCode::kCorrupt, "unknown attempt state " + std::to_string(state));
  }
  value.state = static_cast<TargetState>(state);
  RF_TRY(detail::dc_bool(reader, "attempt.completed", value.completed));
  std::uint8_t outcome = 0;
  RF_TRY(dc_u8(reader, "attempt.outcome", outcome));
  if (outcome > static_cast<std::uint8_t>(EvidenceOutcome::kInconclusive)) {
    return make_status(StatusCode::kCorrupt, "unknown attempt outcome");
  }
  value.outcome = static_cast<EvidenceOutcome>(outcome);
  std::uint64_t cancelled = 0;
  RF_TRY(detail::dc_u64(reader, "attempt.cancelled_epoch", cancelled));
  value.cancelled_epoch = AttemptEpoch{cancelled};
  std::uint64_t sequence = 0;
  RF_TRY(detail::dc_u64(reader, "attempt.highest_sequence", sequence));
  value.highest_sequence = Sequence{sequence};
  RF_TRY(dc_u32(reader, "attempt.accepted_evidence", value.accepted_evidence));
  RF_TRY(dc_u32(reader, "attempt.duplicate_evidence", value.duplicate_evidence));
  RF_TRY(dc_u32(reader, "attempt.out_of_order_evidence", value.out_of_order_evidence));
  if (value.completed && !is_terminal(value.state)) {
    return make_status(StatusCode::kCorrupt,
                       "attempt is marked completed but its state is " +
                           std::string(to_string(value.state)));
  }
  return value;
}

}  // namespace rollout_fabric
