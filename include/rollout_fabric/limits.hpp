// Rollout Fabric - bounded resource surfaces.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Nothing in this runtime grows without a ceiling. Every collection that an
// operator, a plan, a peer process or a journal can influence is sized against
// a bound carried in this struct, and the bound itself is validated before the
// runtime accepts it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "rollout_fabric/status.hpp"

namespace rollout_fabric {

struct RuntimeLimits {
  // --- Plan and cohort structural bounds ---
  std::uint32_t max_stages_per_plan = 64;
  std::uint32_t max_targets_per_cohort = 4096;
  std::uint32_t max_total_targets = 65536;
  std::uint32_t max_actions_per_stage = 64;
  std::uint32_t max_selector_terms = 4096;
  std::uint32_t max_labels_per_target = 32;
  std::uint32_t max_label_key_bytes = 64;
  std::uint32_t max_label_value_bytes = 256;
  std::uint32_t max_name_bytes = 128;
  std::uint32_t max_description_bytes = 1024;
  // Bound on a whole upstream document (a plan or an inventory). These are
  // legitimately large; the ceiling exists so that a hostile or truncated
  // upload cannot make the parser allocate without limit.
  std::uint32_t max_document_bytes = 4u << 20;

  // --- Attempt and evidence bounds ---
  std::uint32_t max_attempts_per_target_per_stage = 16;
  std::uint32_t max_evidence_per_attempt = 64;
  std::uint32_t max_metrics_per_evidence = 32;
  std::uint32_t max_evidence_payload_bytes = 4096;
  std::uint32_t max_metric_name_bytes = 64;
  std::uint32_t max_metric_text_bytes = 256;
  std::uint32_t max_dedup_entries_per_attempt = 256;
  std::uint32_t max_out_of_order_evidence_per_attempt = 64;
  std::uint32_t max_gate_evaluations_per_stage = 4096;

  // --- Decision and explanation bounds ---
  std::uint32_t max_rejected_alternatives = 32;
  std::uint32_t max_rationale_bytes = 8192;
  // Decisions retained inside a rollout snapshot. The journal keeps every decision
  // as its own record, so this bounds the size of a snapshot without losing the
  // audit trail.
  std::uint32_t max_decision_history = 64;

  // --- Persistence bounds ---
  std::uint64_t max_journal_record_bytes = 1u << 20;
  std::uint64_t max_journal_bytes = 1ull << 31;         // 2 GiB hard ceiling
  std::uint64_t snapshot_trigger_records = 4096;        // compaction cadence
  std::uint32_t max_snapshot_bytes = 1u << 28;          // 256 MiB
  std::uint32_t journal_format_version = 1;

  // --- Transport bounds ---
  std::uint32_t max_frame_bytes = 1u << 20;
  std::uint32_t max_connections = 256;
  std::uint32_t max_queued_frames_per_connection = 1024;
  std::uint32_t max_workers = 256;
  std::uint32_t max_message_bytes = 1u << 20;
  std::uint32_t heartbeat_interval_millis = 1000;
  std::uint32_t connection_idle_timeout_millis = 30000;

  // --- Dispatch bounds ---
  std::uint32_t max_dispatch_batch = 512;
  std::uint32_t max_concurrent_attempts = 4096;
  std::uint32_t max_idempotency_entries = 65536;

  // --- Freshness defaults ---
  std::uint32_t default_evidence_max_age_millis = 30000;
  std::uint32_t max_future_skew_millis = 5000;

  // Rejects configurations that are internally inconsistent or that exceed the
  // runtime's own hard ceilings.
  [[nodiscard]] Status validate() const;
};

// Hard ceilings that no configuration may exceed, independent of what an
// operator asks for.
inline constexpr std::uint64_t kHardMaxJournalRecordBytes = 64ull << 20;
inline constexpr std::uint64_t kHardMaxFrameBytes = 64ull << 20;
inline constexpr std::uint64_t kHardMaxSnapshotBytes = 4ull << 30;
inline constexpr std::uint32_t kHardMaxTargets = 1u << 20;
inline constexpr std::uint32_t kHardMaxStages = 4096;

// The journal format this build can read and write. A configuration naming any
// other version is rejected rather than silently downgraded.
inline constexpr std::uint32_t kSupportedJournalFormatVersion = 1;

[[nodiscard]] const RuntimeLimits& default_limits() noexcept;

}  // namespace rollout_fabric
