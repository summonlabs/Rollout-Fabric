// Rollout Fabric - the orchestration state machine.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Threading contract, stated once and relied on everywhere:
//
//   * An Orchestrator instance is owned by exactly one thread. It holds no
//     mutex, takes no lock, and never blocks on another thread.
//   * Adapters are polled by that same thread. They may hand evidence over from
//     inside poll(); that evidence is queued and applied after poll() returns,
//     so an adapter can never re-enter the state machine recursively.
//   * Concurrency in this system lives in separate operating system processes,
//     which is where it is actually proved.
//
// There is therefore no lock-ordering hazard to audit inside the runtime: there
// are no locks in the state machine. The audit is about reentrancy and about
// shutdown order, both of which are covered by the test suite.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rollout_fabric/adapters.hpp"
#include "rollout_fabric/decision.hpp"
#include "rollout_fabric/evidence.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/journal.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/plan.hpp"
#include "rollout_fabric/rollout.hpp"
#include "rollout_fabric/selector.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/target.hpp"
#include "rollout_fabric/time.hpp"

namespace rollout_fabric {

enum class CommandKind : std::uint8_t {
  kCreate = 0,
  kValidate = 1,
  kArm = 2,
  kStart = 3,
  kPause = 4,
  kResume = 5,
  kAbort = 6,
  kApproveGate = 7,
  kRetire = 8,
  // Creates a new rollout generation: the only way the membership of an armed
  // stage may change.
  kRegenerate = 9,
  // Read-only commands. They mutate nothing and are served from the same
  // reactor, so an operator can observe a rollout without a second channel.
  kStatus = 10,
  kExplain = 11,
  kInspectStage = 12,
  kInspectCohort = 13,
  kListEvents = 14,
};

[[nodiscard]] std::string_view to_string(CommandKind kind) noexcept;

// Authority under which a command is accepted. A command whose incarnation or
// controller epoch does not match the running controller is fenced before it
// can touch any state.
struct CommandContext {
  AuthorityId authority{};
  OperatorId operator_id{};
  std::string operator_name;
  IncarnationId incarnation{};
  EpochCounter controller_epoch{0};
  // Set when the command originates from a remote client that named the
  // generation and revision it believed were current. Unset means "do not
  // check", which is only used by the local in-process API.
  GenerationId expected_generation{};
  Revision expected_revision{0};
  bool checked_generation = false;
  bool checked_revision = false;
};

struct OrchestratorDeps {
  RuntimeLimits limits{};
  const Clock* clock = nullptr;
  // Optional. When absent the orchestrator runs purely in memory, which is how
  // the unit and property suites drive it.
  JournalWriter* journal = nullptr;
  IExecutionAdapter* execution = nullptr;
  IHealthAdapter* health = nullptr;
  IdFactory ids{};
  IncarnationId incarnation{};
  EpochCounter controller_epoch{0};
  TargetInventory inventory{};
};

struct TickReport {
  std::uint32_t dispatched = 0;
  std::uint32_t completed = 0;
  std::uint32_t transitions = 0;
  std::uint32_t gates_evaluated = 0;
  std::uint32_t decisions = 0;
};

struct RecoveryOutcome {
  std::uint32_t rollouts_restored = 0;
  std::uint32_t rollouts_skipped = 0;
  std::uint32_t outstanding_attempts = 0;
  std::uint32_t stale_incarnations = 0;
  RecoveryReport journal{};
  std::string detail;
};

// Counters exposed for observation and for tests that assert on fencing.
struct OrchestratorCounters {
  std::uint64_t evidence_accepted = 0;
  std::uint64_t evidence_duplicate = 0;
  std::uint64_t evidence_out_of_order = 0;
  std::uint64_t evidence_stale_generation = 0;
  std::uint64_t evidence_stale_attempt = 0;
  std::uint64_t evidence_stale_incarnation = 0;
  std::uint64_t evidence_rejected = 0;
  std::uint64_t dispatch_refused = 0;
  std::uint64_t journal_appends = 0;
  std::uint64_t compactions = 0;
};

class Orchestrator : public IEvidenceSink {
 public:
  explicit Orchestrator(OrchestratorDeps deps);
  ~Orchestrator() override;

  Orchestrator(const Orchestrator&) = delete;
  Orchestrator& operator=(const Orchestrator&) = delete;

  // --- Lifecycle commands -------------------------------------------------

  [[nodiscard]] Result<RolloutId> create_rollout(const ChangePlan& plan,
                                                 const CommandContext& context,
                                                 std::string* detail);
  [[nodiscard]] Status arm(const RolloutId& rollout, const CommandContext& context, std::string* detail);
  [[nodiscard]] Status start(const RolloutId& rollout, const CommandContext& context, std::string* detail);
  [[nodiscard]] Status pause(const RolloutId& rollout, const CommandContext& context, std::string reason,
                             std::string* detail);
  [[nodiscard]] Status resume(const RolloutId& rollout, const CommandContext& context, std::string* detail);
  [[nodiscard]] Status abort(const RolloutId& rollout, const CommandContext& context, std::string reason,
                             std::string* detail);
  [[nodiscard]] Status approve_gate(const RolloutId& rollout, const GateId& gate,
                                    const CommandContext& context, std::string* detail);
  [[nodiscard]] Status retire(const RolloutId& rollout, const CommandContext& context, std::string* detail);
  [[nodiscard]] Status regenerate(const RolloutId& rollout, const CommandContext& context,
                                  std::string* detail);
  [[nodiscard]] Status set_inventory(const TargetInventory& inventory);

  // --- Drive --------------------------------------------------------------

  // One orchestration step: poll adapters, admit the evidence they produced,
  // evaluate gates, dispatch what policy permits, and persist any mutation.
  [[nodiscard]] TickReport tick();
  [[nodiscard]] const Status& storage_status() const noexcept;
  void note_shutdown() noexcept;

  // Re-queries the execution adapter about attempts that were outstanding
  // before this controller incarnation started. The adapter is asked before
  // anything is dispatched again, and only genuinely unstarted attempts become
  // eligible a second time.
  [[nodiscard]] Status reconcile_outstanding(const RolloutId& rollout, std::string* detail);

  // --- Observation --------------------------------------------------------

  [[nodiscard]] const Rollout* find(const RolloutId& id) const noexcept;
  [[nodiscard]] std::vector<const Rollout*> rollouts() const;
  [[nodiscard]] const TargetInventory& inventory() const noexcept;
  [[nodiscard]] const RuntimeLimits& limits() const noexcept;
  [[nodiscard]] const IncarnationId& incarnation() const noexcept;
  [[nodiscard]] EpochCounter controller_epoch() const noexcept;
  [[nodiscard]] const Clock& clock() const noexcept;
  [[nodiscard]] const ChangePlan* plan_for(const RolloutId& id) const noexcept;
  [[nodiscard]] const Attempt* find_attempt(const AttemptId& id) const noexcept;
  [[nodiscard]] std::vector<Attempt> outstanding_attempts(const RolloutId& id) const;
  [[nodiscard]] const OrchestratorCounters& counters() const noexcept;
  [[nodiscard]] const std::vector<AdapterEvent>& recent_adapter_events() const;

  [[nodiscard]] Result<std::string> explain(const RolloutId& rollout,
                                            const std::optional<DecisionId>& decision) const;
  [[nodiscard]] Result<StageRuntime> stage_view(const RolloutId& rollout, const StageId& stage) const;
  [[nodiscard]] Result<CohortMembership> cohort_view(const RolloutId& rollout, const StageId& stage) const;
  [[nodiscard]] std::vector<Decision> recent_decisions(const RolloutId& rollout, std::size_t count) const;

  // Restores one rollout recovered from durable state, together with the plan
  // it was created from. Fenced against identifier collisions and against a
  // plan whose digest does not match the one the rollout recorded.
  [[nodiscard]] Status restore(Rollout rollout, const ChangePlan& plan, const RecoveryReport& report);

  // --- IEvidenceSink ------------------------------------------------------

  void on_evidence(EvidenceRecord record) override;
  void on_adapter_event(AdapterEvent event) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// --- Pure helpers shared by the state machine, the tests and the tools -----

struct DispatchPlanEntry {
  TargetId target{};
  AttemptEpoch epoch{};
  ActionId action{};
  std::uint32_t attempt_number = 1;
  std::uint32_t rollback_step = 0;
};

struct DispatchPlan {
  std::vector<DispatchPlanEntry> admitted;
  std::vector<RejectedAlternative> rejected;
  std::uint32_t changing_before = 0;
};

[[nodiscard]] DispatchPlan plan_dispatch(const Rollout& rollout, const StageRuntime& stage,
                                         const TargetInventory& inventory,
                                         const RuntimeLimits& limits);

// Blast radius accounting for one candidate target. Refusal always names the
// dimension that was exhausted.
[[nodiscard]] Status blast_radius_admits(const Rollout& rollout, const StageRuntime& stage,
                                         const TargetInventory& inventory, const TargetId& candidate,
                                         std::string* reason);

[[nodiscard]] GateEvaluation evaluate_health_gate(const StageRuntime& stage,
                                                  const HealthGatePolicy& policy,
                                                  const HealthSnapshot& snapshot, Timestamp now,
                                                  const RuntimeLimits& limits);

[[nodiscard]] GateEvaluation evaluate_evidence_gate(const StageRuntime& stage,
                                                    const EvidenceGatePolicy& policy,
                                                    const TargetEvidenceRule& rule, Timestamp now,
                                                    const RuntimeLimits& limits);

[[nodiscard]] GateEvaluation evaluate_advance_gate(const StageRuntime& stage, Timestamp now,
                                                   const RuntimeLimits& limits);

// The action a stage dispatches for its normal work, and the compensation
// action at a given step. Deterministic; used by the state machine and by the
// dispatch planner so both agree on what would be sent.
[[nodiscard]] const ActionSpec* primary_action(const StageRuntime& stage) noexcept;
[[nodiscard]] const ActionSpec* compensation_action(const StageRuntime& stage,
                                                    std::uint32_t step) noexcept;

}  // namespace rollout_fabric
