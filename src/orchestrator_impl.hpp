// Rollout Fabric - internal orchestrator state.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Private to the library: not installed. The state machine is split across
// three translation units that share this definition; the public class stays a
// pimpl so that none of it appears in the installed headers.
#pragma once

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "rollout_fabric/orchestrator.hpp"

namespace rollout_fabric {

struct Orchestrator::Impl {
  RuntimeLimits limits{};
  const Clock* clock = nullptr;
  JournalWriter* journal = nullptr;
  IExecutionAdapter* execution = nullptr;
  IHealthAdapter* health = nullptr;
  IdFactory ids{};
  IncarnationId incarnation{};
  EpochCounter controller_epoch{0};
  TargetInventory inventory{};
  std::vector<std::unique_ptr<Rollout>> rollouts;
  std::vector<std::unique_ptr<ChangePlan>> plans;

  // Transient state. None of it is durable: it is rebuilt from the journal and
  // from the execution adapter when a new incarnation takes over.
  std::vector<EvidenceRecord> pending_evidence;
  std::vector<AdapterEvent> adapter_events;
  std::map<AttemptId, Attempt> attempts;
  std::deque<AttemptId> attempt_order;
  std::map<AttemptId, std::vector<EvidenceRecord>> parked_evidence;
  std::deque<Digest256> idempotency_order;
  std::set<Digest256> idempotency_keys;
  std::map<RolloutId, Timestamp> last_probe_at;
  std::uint64_t health_request_id = 0;
  // Digest of the last snapshot written for each rollout, so that a tick which
  // changed nothing does not grow the journal.
  std::map<RolloutId, Digest256> last_persisted;
  Status storage_status = Status::success();
  bool storage_failed = false;
  bool shutting_down = false;
  OrchestratorCounters counters{};

  // --- lookups ---
  [[nodiscard]] Rollout* mutable_rollout(const RolloutId& id) noexcept;
  [[nodiscard]] const Rollout* find(const RolloutId& id) const noexcept;
  [[nodiscard]] const ChangePlan* plan_for(const RolloutId& id) const noexcept;
  [[nodiscard]] std::size_t rollout_index(const RolloutId& id) const noexcept;

  // --- fences ---
  [[nodiscard]] Status check_authority(const CommandContext& context) const;
  [[nodiscard]] Status check_fences(const Rollout& rollout, const CommandContext& context) const;

  // --- persistence ---
  Status append_journal(JournalRecordType type, Timestamp now, std::span<const std::byte> payload);
  Status persist_rollout(const Rollout& rollout, Timestamp now);
  Status persist_decision(const Decision& decision, Timestamp now);
  Status maybe_compact(Timestamp now);
  Status transition(Rollout& rollout, RolloutState to, std::string reason, Timestamp now,
                    TickReport* report);
  Status stage_transition(Rollout& rollout, StageRuntime& stage, StageState to, Timestamp now,
                          TickReport* report);
  void commit_decision(Rollout& rollout, Decision decision, Timestamp now);

  // --- bookkeeping ---
  void note_idempotency_key(const Digest256& key);
  void note_attempt(const Attempt& attempt);
  void note_adapter_event(const AdapterEvent& event);

  // --- evidence ----------------------------------------------------------
  void drain_pending(TickReport& report, Timestamp now);
  [[nodiscard]] AdmissionResult admit(const EvidenceRecord& record, Timestamp now,
                                      const Rollout** rollout_out, const ChangePlan** plan_out);
  Status apply_evidence(Rollout& rollout, const ChangePlan& plan, const EvidenceRecord& record,
                        Timestamp now, TickReport& report);
  void apply_health(const EvidenceRecord& record, Rollout& rollout, TargetRuntime& target,
                    Timestamp now);
  // Telemetry is routed by target across every rollout that holds it. It is not
  // fenced by attempt, generation or incarnation, because it describes
  // infrastructure rather than a change; freshness and the attesting source are
  // what a gate checks instead.
  bool apply_telemetry(const EvidenceRecord& record, Timestamp now);
  void apply_terminal_execution(const EvidenceRecord& record, Rollout& rollout, StageRuntime& stage,
                                TargetRuntime& target, Attempt& attempt, Timestamp now,
                                TickReport& report);
  void apply_compensation(const EvidenceRecord& record, Rollout& rollout, StageRuntime& stage,
                          TargetRuntime& target, Timestamp now, TickReport& report);
  void retry_or_exhaust(Rollout& rollout, StageRuntime& stage, TargetRuntime& target, Timestamp now,
                        TickReport& report);

  // --- state machine (orchestrator_advance.cpp) ---------------------------
  void advance(Rollout& rollout, const ChangePlan& plan, Timestamp now, TickReport& report);
  Status begin_stage(Rollout& rollout, const ChangePlan& plan, StageRuntime& stage, Timestamp now,
                     TickReport& report);
  Status run_stage(Rollout& rollout, const ChangePlan& plan, StageRuntime& stage, Timestamp now,
                   TickReport& report);
  Status dispatch_stage(Rollout& rollout, const ChangePlan& plan, StageRuntime& stage, Timestamp now,
                        TickReport& report);
  Status create_and_dispatch(Rollout& rollout, StageRuntime& stage, const TargetRuntime& target,
                             const ActionSpec& action, std::uint32_t rollback_step,
                             AttemptEpoch epoch, Timestamp now, TickReport& report);
  Status complete_stage(Rollout& rollout, StageRuntime& stage, Timestamp now, TickReport& report,
                          bool approved);
  Status fail_stage(Rollout& rollout, StageRuntime& stage, std::string reason, Timestamp now,
                    TickReport& report);
  Status fail_rollout(Rollout& rollout, std::string reason, Timestamp now, TickReport& report);
  Status begin_rollback(Rollout& rollout, std::string reason, Timestamp now, TickReport& report);
  Status run_rollback(Rollout& rollout, StageRuntime& stage, TargetRuntime& target, Timestamp now,
                      TickReport& report);
  Status finish_rollback(Rollout& rollout, Timestamp now, TickReport& report);
  void open_gate(Rollout& rollout, StageRuntime& stage, GateKind kind, Timestamp now);
  Status cancel_in_flight(Rollout& rollout, StageRuntime& stage, Timestamp now, TickReport& report);
  void probe_health(Rollout& rollout, const StageRuntime& stage, Timestamp now);
  [[nodiscard]] bool health_gate_holds(const StageRuntime& stage, Timestamp now);
};

// Shared helpers used by more than one of the state-machine translation units.
namespace orchestrator_detail {

[[nodiscard]] std::uint64_t failure_budget(const StageRuntime& stage);
[[nodiscard]] std::string short_hex(const RolloutId& id);
[[nodiscard]] std::string short_hex(const StageId& id);
[[nodiscard]] std::string short_hex(const TargetId& id);
[[nodiscard]] std::string short_hex(const AttemptId& id);
[[nodiscard]] std::string short_hex(const EvidenceId& id);
[[nodiscard]] std::string short_hex(const GateId& id);
[[nodiscard]] TargetId resolve_target(const Attempt& attempt);
[[nodiscard]] bool rollout_has_compensation(const Rollout& rollout);

}  // namespace orchestrator_detail

}  // namespace rollout_fabric
