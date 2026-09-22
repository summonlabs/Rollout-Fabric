// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/limits.hpp"

#include <string>

namespace rollout_fabric {
namespace {

[[nodiscard]] Status require(bool condition, std::string message) {
  if (condition) {
    return Status::success();
  }
  return make_status(StatusCode::kInvalidArgument, std::move(message));
}

}  // namespace

const RuntimeLimits& default_limits() noexcept {
  static const RuntimeLimits limits{};
  return limits;
}

Status RuntimeLimits::validate() const {
  Status status = require(max_stages_per_plan > 0 && max_stages_per_plan <= kHardMaxStages,
                          "max_stages_per_plan must be in [1, " + std::to_string(kHardMaxStages) + "]");
  if (!status.ok()) {
    return status;
  }
  status = require(max_total_targets > 0 && max_total_targets <= kHardMaxTargets,
                   "max_total_targets must be in [1, " + std::to_string(kHardMaxTargets) + "]");
  if (!status.ok()) {
    return status;
  }
  status = require(max_targets_per_cohort > 0 && max_targets_per_cohort <= max_total_targets,
                   "max_targets_per_cohort must be in [1, max_total_targets]");
  if (!status.ok()) {
    return status;
  }
  status = require(max_actions_per_stage > 0, "max_actions_per_stage must be positive");
  if (!status.ok()) {
    return status;
  }
  status = require(max_selector_terms > 0, "max_selector_terms must be positive");
  if (!status.ok()) {
    return status;
  }
  status = require(max_name_bytes >= 8 && max_description_bytes >= max_name_bytes,
                   "max_name_bytes must be at least 8 and no larger than max_description_bytes");
  if (!status.ok()) {
    return status;
  }
  status = require(max_attempts_per_target_per_stage > 0,
                   "max_attempts_per_target_per_stage must be positive");
  if (!status.ok()) {
    return status;
  }
  status = require(max_evidence_per_attempt > 0 && max_dedup_entries_per_attempt > 0,
                   "evidence and dedup bounds must be positive");
  if (!status.ok()) {
    return status;
  }
  status = require(max_rejected_alternatives > 0 && max_decision_history > 0,
                   "decision bounds must be positive");
  if (!status.ok()) {
    return status;
  }
  status = require(max_journal_record_bytes >= 4096 && max_journal_record_bytes <= kHardMaxJournalRecordBytes,
                   "max_journal_record_bytes must be in [4096, " +
                       std::to_string(kHardMaxJournalRecordBytes) + "]");
  if (!status.ok()) {
    return status;
  }
  status = require(max_journal_bytes >= max_journal_record_bytes,
                   "max_journal_bytes must be at least max_journal_record_bytes");
  if (!status.ok()) {
    return status;
  }
  status = require(max_snapshot_bytes >= max_journal_record_bytes &&
                       max_snapshot_bytes <= kHardMaxSnapshotBytes,
                   "max_snapshot_bytes must be at least max_journal_record_bytes and no more than " +
                       std::to_string(kHardMaxSnapshotBytes));
  if (!status.ok()) {
    return status;
  }
  status = require(snapshot_trigger_records > 0, "snapshot_trigger_records must be positive");
  if (!status.ok()) {
    return status;
  }
  status = require(journal_format_version == kSupportedJournalFormatVersion,
                   "journal_format_version must name a format this build understands");
  if (!status.ok()) {
    return status;
  }
  status = require(max_frame_bytes >= 4096 && max_frame_bytes <= kHardMaxFrameBytes,
                   "max_frame_bytes must be in [4096, " + std::to_string(kHardMaxFrameBytes) + "]");
  if (!status.ok()) {
    return status;
  }
  status = require(max_message_bytes > 0 && max_message_bytes <= max_frame_bytes,
                   "max_message_bytes must be positive and no larger than max_frame_bytes");
  if (!status.ok()) {
    return status;
  }
  status = require(max_connections > 0 && max_workers > 0 && max_workers <= max_connections,
                   "max_workers must be in [1, max_connections]");
  if (!status.ok()) {
    return status;
  }
  status = require(max_queued_frames_per_connection > 0,
                   "max_queued_frames_per_connection must be positive");
  if (!status.ok()) {
    return status;
  }
  status = require(max_dispatch_batch > 0 && max_dispatch_batch <= max_concurrent_attempts,
                   "max_dispatch_batch must be in [1, max_concurrent_attempts]");
  if (!status.ok()) {
    return status;
  }
  status = require(max_idempotency_entries > 0, "max_idempotency_entries must be positive");
  if (!status.ok()) {
    return status;
  }
  status = require(default_evidence_max_age_millis > 0,
                   "default_evidence_max_age_millis must be positive");
  if (!status.ok()) {
    return status;
  }
  status = require(max_document_bytes >= 4096, "max_document_bytes must be at least 4096");
  if (!status.ok()) {
    return status;
  }
  status = require(max_gate_evaluations_per_stage > 0, "max_gate_evaluations_per_stage must be positive");
  if (!status.ok()) {
    return status;
  }
  status = require(max_out_of_order_evidence_per_attempt > 0,
                   "max_out_of_order_evidence_per_attempt must be positive");
  if (!status.ok()) {
    return status;
  }
  return Status::success();
}

}  // namespace rollout_fabric
