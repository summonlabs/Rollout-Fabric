// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Commands, evidence admission, the observation API and durable recovery.
// The advancing state machine lives in orchestrator_advance.cpp.
#include <algorithm>
#include <string>
#include <utility>

#include "orchestrator_impl.hpp"
#include "rollout_fabric/persistence.hpp"

namespace rollout_fabric {

std::string_view to_string(CommandKind kind) noexcept {
  switch (kind) {
    case CommandKind::kCreate: return "create";
    case CommandKind::kValidate: return "validate";
    case CommandKind::kArm: return "arm";
    case CommandKind::kStart: return "start";
    case CommandKind::kPause: return "pause";
    case CommandKind::kResume: return "resume";
    case CommandKind::kAbort: return "abort";
    case CommandKind::kApproveGate: return "approve_gate";
    case CommandKind::kRetire: return "retire";
    case CommandKind::kRegenerate: return "regenerate";
    case CommandKind::kStatus: return "status";
    case CommandKind::kExplain: return "explain";
    case CommandKind::kInspectStage: return "inspect_stage";
    case CommandKind::kInspectCohort: return "inspect_cohort";
    case CommandKind::kListEvents: return "list_events";
  }
  return "unknown";
}

namespace orchestrator_detail {

std::uint64_t failure_budget(const StageRuntime& stage) {
  const std::uint64_t by_count = stage.policy.failure_budget_targets;
  const std::uint64_t by_fraction =
      stage.policy.failure_budget_fraction.apply_floor(static_cast<std::uint64_t>(stage.targets.size()));
  return std::max(by_count, by_fraction);
}

std::string short_hex(const RolloutId& id) { return id.to_hex().substr(0, 12); }
std::string short_hex(const StageId& id) { return id.to_hex().substr(0, 12); }
std::string short_hex(const TargetId& id) { return id.to_hex().substr(0, 12); }
std::string short_hex(const AttemptId& id) { return id.to_hex().substr(0, 12); }
std::string short_hex(const EvidenceId& id) { return id.to_hex().substr(0, 12); }
std::string short_hex(const GateId& id) { return id.to_hex().substr(0, 12); }

bool rollout_has_compensation(const Rollout& rollout) {
  for (const StageRuntime& stage : rollout.stages) {
    if (stage.policy.rollback_supported && !stage.policy.compensation_actions.empty()) {
      return true;
    }
  }
  return false;
}

}  // namespace orchestrator_detail

// ---------------------------------------------------------------------------
// Impl: lookups and fences
// ---------------------------------------------------------------------------

Rollout* Orchestrator::Impl::mutable_rollout(const RolloutId& id) noexcept {
  for (auto& entry : rollouts) {
    if (entry->id == id) {
      return entry.get();
    }
  }
  return nullptr;
}

const Rollout* Orchestrator::Impl::find(const RolloutId& id) const noexcept {
  for (const auto& entry : rollouts) {
    if (entry->id == id) {
      return entry.get();
    }
  }
  return nullptr;
}

const ChangePlan* Orchestrator::Impl::plan_for(const RolloutId& id) const noexcept {
  for (std::size_t index = 0; index < rollouts.size(); ++index) {
    if (rollouts[index]->id == id) {
      return plans[index].get();
    }
  }
  return nullptr;
}

std::size_t Orchestrator::Impl::rollout_index(const RolloutId& id) const noexcept {
  for (std::size_t index = 0; index < rollouts.size(); ++index) {
    if (rollouts[index]->id == id) {
      return index;
    }
  }
  return rollouts.size();
}

Status Orchestrator::Impl::check_authority(const CommandContext& context) const {
  if (!context.incarnation.is_nil() && !(context.incarnation == incarnation)) {
    return make_status(StatusCode::kStaleEpoch,
                       "the command names controller incarnation " +
                           context.incarnation.to_hex().substr(0, 12) + ", this controller is " +
                           incarnation.to_hex().substr(0, 12));
  }
  if (value_of(context.controller_epoch) != 0 &&
      value_of(context.controller_epoch) != value_of(controller_epoch)) {
    return make_status(StatusCode::kStaleEpoch,
                       "the command names controller epoch " +
                           std::to_string(value_of(context.controller_epoch)) +
                           ", this controller holds epoch " +
                           std::to_string(value_of(controller_epoch)));
  }
  return Status::success();
}

Status Orchestrator::Impl::check_fences(const Rollout& rollout, const CommandContext& context) const {
  if (context.checked_generation && !(context.expected_generation == rollout.generation)) {
    return make_status(StatusCode::kStaleGeneration,
                       "the command was issued against generation " +
                           context.expected_generation.to_hex().substr(0, 12) +
                           " but the rollout is at generation " +
                           rollout.generation.to_hex().substr(0, 12));
  }
  if (context.checked_revision &&
      value_of(context.expected_revision) != value_of(rollout.revision)) {
    return make_status(StatusCode::kStaleRevision,
                       "the command was issued against revision " +
                           std::to_string(value_of(context.expected_revision)) +
                           " but the rollout is at revision " +
                           std::to_string(value_of(rollout.revision)));
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// Impl: persistence
// ---------------------------------------------------------------------------

Status Orchestrator::Impl::append_journal(JournalRecordType type, Timestamp now,
                                          std::span<const std::byte> payload) {
  if (journal == nullptr) {
    return Status::success();
  }
  if (storage_failed) {
    return storage_status;
  }
  const Status status = journal->append(type, controller_epoch, incarnation, now, payload);
  if (!status.ok()) {
    storage_failed = true;
    storage_status = make_status(status.code(), "journal append failed: " + status.message());
    return storage_status;
  }
  ++counters.journal_appends;
  return Status::success();
}

Status Orchestrator::Impl::persist_rollout(const Rollout& rollout, Timestamp now) {
  // A tick that changed nothing must not grow durable state. The digest of the
  // last snapshot is compared first, which keeps journal growth proportional to
  // real progress rather than to the tick rate.
  const Digest256 digest = rollout_digest(rollout);
  const auto previous = last_persisted.find(rollout.id);
  if (previous != last_persisted.end() && digest_equal(previous->second, digest)) {
    return Status::success();
  }
  const std::vector<std::byte> payload = encode_rollout_payload(rollout, limits);
  if (payload.size() > limits.max_journal_record_bytes) {
    return make_status(StatusCode::kResourceExhausted,
                       "rollout snapshot is " + std::to_string(payload.size()) +
                           " bytes, above max_journal_record_bytes; reduce the cohort size or raise "
                           "the limit deliberately");
  }
  const Status status = append_journal(JournalRecordType::kRolloutSnapshot, now, payload);
  if (status.ok()) {
    last_persisted[rollout.id] = digest;
  }
  return status;
}

Status Orchestrator::Impl::persist_decision(const Decision& decision, Timestamp now) {
  ByteWriter writer;
  encode(writer, decision);
  return append_journal(JournalRecordType::kDecisionRecorded, now, writer.span());
}

Status Orchestrator::Impl::maybe_compact(Timestamp now) {
  if (journal == nullptr || !journal->is_open()) {
    return Status::success();
  }
  if (journal->records_since_snapshot() < limits.snapshot_trigger_records) {
    return Status::success();
  }
  std::vector<JournalRecord> records;
  const auto push = [&records](JournalRecordType type, const std::vector<std::byte>& payload) {
    JournalRecord record;
    record.type = type;
    record.payload = payload;
    records.push_back(std::move(record));
  };
  push(JournalRecordType::kInventoryStored, encode_inventory_payload(inventory, limits));
  for (std::size_t index = 0; index < rollouts.size(); ++index) {
    push(JournalRecordType::kPlanStored, encode_plan_payload(*plans[index], limits));
    push(JournalRecordType::kRolloutSnapshot, encode_rollout_payload(*rollouts[index], limits));
  }
  ++counters.compactions;
  const Status status = journal->rewrite(records);
  if (!status.ok()) {
    storage_failed = true;
    storage_status = make_status(status.code(), "journal compaction failed: " + status.message());
    return storage_status;
  }
  journal->note_snapshot();
  (void)now;
  return Status::success();
}

Status Orchestrator::Impl::transition(Rollout& rollout, RolloutState to, std::string reason,
                                      Timestamp now, TickReport* report) {
  if (rollout.state == to) {
    return Status::success();
  }
  if (!legal_transition(rollout.state, to)) {
    return make_status(StatusCode::kConflict,
                       "illegal rollout transition from " + std::string(to_string(rollout.state)) +
                           " to " + std::string(to_string(to)));
  }
  rollout.state = to;
  rollout.state_reason = std::move(reason);
  rollout.updated_at = now;
  rollout.revision = next(rollout.revision);
  if (to == RolloutState::kArmed) {
    rollout.armed_at = now;
  }
  if (is_terminal(to)) {
    rollout.completed_at = now;
  }
  if (report != nullptr) {
    ++report->transitions;
  }
  return persist_rollout(rollout, now);
}

Status Orchestrator::Impl::stage_transition(Rollout& rollout, StageRuntime& stage, StageState to,
                                            Timestamp now, TickReport* report) {
  if (stage.state == to) {
    return Status::success();
  }
  if (!legal_transition(stage.state, to)) {
    return make_status(StatusCode::kConflict,
                       "illegal stage transition from " + std::string(to_string(stage.state)) +
                           " to " + std::string(to_string(to)) + " on stage " +
                           orchestrator_detail::short_hex(stage.id));
  }
  stage.state = to;
  stage.last_progress_at = now;
  if (to == StageState::kRunning && stage.started_at.is_nil()) {
    stage.started_at = now;
  }
  if (to == StageState::kGated) {
    stage.gated_at = now;
  }
  if (is_terminal(to)) {
    stage.completed_at = now;
  }
  rollout.updated_at = now;
  rollout.revision = next(rollout.revision);
  if (report != nullptr) {
    ++report->transitions;
  }
  return persist_rollout(rollout, now);
}

void Orchestrator::Impl::commit_decision(Rollout& rollout, Decision decision, Timestamp now) {
  const Decision copy = decision;
  rollout.record_decision(std::move(decision), limits);
  const Status status = persist_decision(copy, now);
  if (!status.ok()) {
    storage_failed = true;
    storage_status = status;
  }
}

void Orchestrator::Impl::note_idempotency_key(const Digest256& key) {
  if (!idempotency_keys.insert(key).second) {
    return;
  }
  idempotency_order.push_back(key);
  while (idempotency_order.size() > limits.max_idempotency_entries) {
    idempotency_keys.erase(idempotency_order.front());
    idempotency_order.pop_front();
  }
}

void Orchestrator::Impl::note_attempt(const Attempt& attempt) {
  if (attempts.find(attempt.id) == attempts.end()) {
    attempt_order.push_back(attempt.id);
  }
  attempts[attempt.id] = attempt;
  const std::size_t bound = static_cast<std::size_t>(limits.max_concurrent_attempts) * 2u + 1u;
  while (attempt_order.size() > bound) {
    const AttemptId oldest = attempt_order.front();
    const auto found = attempts.find(oldest);
    if (found != attempts.end() && found->second.finished()) {
      attempt_order.pop_front();
      attempts.erase(found);
    } else {
      // Everything currently tracked is in flight. Stop trimming rather than
      // evict a live attempt; the admission path bounds new work separately.
      break;
    }
  }
}

void Orchestrator::Impl::note_adapter_event(const AdapterEvent& event) {
  adapter_events.push_back(event);
  if (adapter_events.size() > limits.max_decision_history) {
    adapter_events.erase(adapter_events.begin(),
                         adapter_events.begin() +
                             static_cast<std::ptrdiff_t>(adapter_events.size() -
                                                         limits.max_decision_history));
  }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Orchestrator::Orchestrator(OrchestratorDeps deps) : impl_(std::make_unique<Impl>()) {
  impl_->limits = deps.limits;
  impl_->clock = deps.clock != nullptr ? deps.clock : nullptr;
  impl_->journal = deps.journal;
  impl_->execution = deps.execution;
  impl_->health = deps.health;
  impl_->ids = deps.ids;
  impl_->incarnation = deps.incarnation;
  impl_->controller_epoch = deps.controller_epoch;
  impl_->inventory = std::move(deps.inventory);
  if (impl_->clock == nullptr) {
    // A null clock is a programming error, but throwing here would make the
    // runtime unusable in a noexcept context. A manual clock keeps the object
    // total and time simply does not advance, which the tests detect at once.
    static ManualClock fallback;
    impl_->clock = &fallback;
  }
}

Orchestrator::~Orchestrator() = default;

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

Result<RolloutId> Orchestrator::create_rollout(const ChangePlan& plan, const CommandContext& context,
                                               std::string* detail) {
  Impl& state = *impl_;
  const Status authority = state.check_authority(context);
  if (!authority.ok()) {
    return authority;
  }
  if (state.storage_failed) {
    return state.storage_status;
  }
  ChangePlan working = plan;
  const Status plan_status = working.validate(state.limits);
  if (!plan_status.ok()) {
    return plan_status;
  }
  if (working.id.is_nil()) {
    return make_status(StatusCode::kInvalidArgument, "plan identifier is nil");
  }
  const Timestamp now = state.clock->wall_now();

  Rollout rollout;
  rollout.id = state.ids.next<RolloutId>(IdDomain::kRollout);
  rollout.plan_id = working.id;
  rollout.plan_digest = working.plan_digest();
  rollout.source_digest = working.source_digest;
  rollout.change_id = working.change_id;
  rollout.generation = state.ids.next<GenerationId>(IdDomain::kGeneration);
  rollout.generation_counter = GenerationCounter{1};
  rollout.revision = Revision{1};
  rollout.incarnation = state.incarnation;
  rollout.controller_epoch = state.controller_epoch;
  rollout.state = RolloutState::kCreated;
  rollout.resume_state = RolloutState::kCreated;
  rollout.state_reason = "created from plan " + working.change_id;
  rollout.created_at = now;
  rollout.updated_at = now;
  rollout.created_by = context.authority;
  rollout.created_by_operator = context.operator_id;

  for (const StageId& stage_id : working.order) {
    const StageSpec* spec = working.find_stage(stage_id);
    if (spec == nullptr) {
      return make_status(StatusCode::kInternal, "plan order names a stage the plan does not hold");
    }
    StageRuntime stage;
    stage.id = spec->id;
    stage.state = StageState::kPending;
    stage.ordinal = spec->ordinal;
    stage.predecessors = spec->predecessors;
    stage.policy = spec->policy;
    stage.evidence_rule = spec->evidence_rule;
    stage.actions = spec->actions;
    auto materialize = materialize_cohort(spec->selector, state.inventory, spec->id, rollout.generation,
                                         state.limits, now);
    if (!materialize.ok()) {
      return make_status(materialize.status().code(),
                         "stage '" + spec->name + "': " + materialize.status().message());
    }
    if (!materialize.value().truncated.empty()) {
      return make_status(StatusCode::kLimitExceeded,
                         "stage '" + spec->name + "' selects " +
                             std::to_string(materialize.value().truncated.size() +
                                            materialize.value().membership.members.size()) +
                             " targets, above max_targets_per_cohort; the runtime refuses to "
                             "silently run a partial cohort");
    }
    stage.cohort = materialize.value().membership;
    stage.cohort.id = state.ids.next<CohortId>(IdDomain::kCohort);
    if (stage.cohort.members.empty()) {
      return make_status(StatusCode::kPreconditionFailed,
                         "stage '" + spec->name + "' selects no targets");
    }
    stage.targets.reserve(stage.cohort.members.size());
    for (const TargetId& member : stage.cohort.members) {
      TargetRuntime target;
      target.target = member;
      target.state = TargetState::kPending;
      stage.targets.push_back(std::move(target));
    }
    rollout.stages.push_back(std::move(stage));
  }
  for (const StageRuntime& stage : rollout.stages) {
    StageRuntime* mutable_stage = rollout.find_stage(stage.id);
    for (const StageId& predecessor : stage.predecessors) {
      StageRuntime* predecessor_stage = rollout.find_stage(predecessor);
      if (predecessor_stage != nullptr && mutable_stage != nullptr) {
        predecessor_stage->successors.push_back(stage.id);
      }
    }
  }

  rollout.record_decision(Decision{}, state.limits);
  rollout.decisions.clear();

  std::uint64_t total_targets = 0;
  for (const StageRuntime& stage : rollout.stages) {
    total_targets += stage.cohort.size();
  }

  DecisionBuilder builder(DecisionKind::kValidatePlan, rollout.id, rollout.generation,
                          rollout.revision, state.controller_epoch, state.incarnation);
  builder.authority(context.authority);
  builder.input("change_id", working.change_id);
  builder.input_digest("source_digest", working.source_digest);
  builder.input_digest("plan_digest", rollout.plan_digest);
  builder.input_u64("stages", static_cast<std::uint64_t>(rollout.stages.size()));
  builder.input_u64("targets", static_cast<std::uint64_t>(total_targets));
  builder.policy_digest(rollout.plan_digest);
  builder.select("rollout created and plan validated");
  builder.rationale("the plan parsed, its dependency graph is acyclic, every stage selector matched "
                    "at least one target and no cohort was truncated");
  state.commit_decision(rollout, builder.finish(state.ids, now, state.limits), now);

  const Status validated =
      state.transition(rollout, RolloutState::kValidated, "plan validated and cohorts materialised",
                       now, nullptr);
  if (!validated.ok()) {
    return validated;
  }
  if (state.journal != nullptr) {
    const std::vector<std::byte> plan_payload = encode_plan_payload(working, state.limits);
    const Status stored = state.append_journal(JournalRecordType::kPlanStored, now, plan_payload);
    if (!stored.ok()) {
      return stored;
    }
    const std::vector<std::byte> inventory_payload =
        encode_inventory_payload(state.inventory, state.limits);
    const Status inventory_stored =
        state.append_journal(JournalRecordType::kInventoryStored, now, inventory_payload);
    if (!inventory_stored.ok()) {
      return inventory_stored;
    }
  }

  const RolloutId id = rollout.id;
  state.rollouts.push_back(std::make_unique<Rollout>(std::move(rollout)));
  state.plans.push_back(std::make_unique<ChangePlan>(std::move(working)));
  if (detail != nullptr) {
    *detail = "rollout " + orchestrator_detail::short_hex(id) + " created with " +
              std::to_string(state.rollouts.back()->stages.size()) + " stage(s)";
  }
  return id;
}

// ---------------------------------------------------------------------------
// Commands (continued)
// ---------------------------------------------------------------------------

Status Orchestrator::arm(const RolloutId& rollout_id, const CommandContext& context,
                         std::string* detail) {
  Impl& state = *impl_;
  const Status authority = state.check_authority(context);
  if (!authority.ok()) {
    return authority;
  }
  Rollout* rollout = state.mutable_rollout(rollout_id);
  if (rollout == nullptr) {
    return make_status(StatusCode::kNotFound, "no such rollout");
  }
  const Status fences = state.check_fences(*rollout, context);
  if (!fences.ok()) {
    return fences;
  }
  if (rollout->state != RolloutState::kValidated && rollout->state != RolloutState::kCreated) {
    return make_status(StatusCode::kPreconditionFailed,
                       "a rollout can only be armed from validated or created, it is " +
                           std::string(to_string(rollout->state)));
  }
  const Timestamp now = state.clock->wall_now();

  // Freezing the cohort is what makes membership immutable for the lifetime of
  // this generation. Once frozen, the only way to obtain a different membership
  // is regenerate(), which mints a new GenerationId.
  for (StageRuntime& stage : rollout->stages) {
    const Status frozen = freeze_cohort(stage, now);
    if (!frozen.ok()) {
      return frozen;
    }
  }

  DecisionBuilder builder(DecisionKind::kArmRollout, rollout->id, rollout->generation,
                          rollout->revision, state.controller_epoch, state.incarnation);
  builder.authority(context.authority);
  builder.input_u64("generation", value_of(rollout->generation_counter));
  for (const StageRuntime& stage : rollout->stages) {
    builder.input("stage", orchestrator_detail::short_hex(stage.id));
    builder.input_u64("members", static_cast<std::uint64_t>(stage.cohort.size()));
    builder.input_digest("membership", stage.cohort.membership_digest);
    builder.input_digest("inventory", stage.cohort.inventory_digest);
  }
  builder.select("cohorts frozen; rollout armed");
  builder.rationale("every stage cohort is non-empty and has been frozen against the current "
                    "inventory; membership cannot change without a new rollout generation");
  state.commit_decision(*rollout, builder.finish(state.ids, now, state.limits), now);

  const Status status =
      state.transition(*rollout, RolloutState::kArmed, "cohorts frozen and armed", now, nullptr);
  if (!status.ok()) {
    return status;
  }
  if (detail != nullptr) {
    *detail = "rollout " + orchestrator_detail::short_hex(rollout->id) + " armed at generation " +
              std::to_string(value_of(rollout->generation_counter));
  }
  return Status::success();
}

Status Orchestrator::start(const RolloutId& rollout_id, const CommandContext& context,
                           std::string* detail) {
  Impl& state = *impl_;
  const Status authority = state.check_authority(context);
  if (!authority.ok()) {
    return authority;
  }
  Rollout* rollout = state.mutable_rollout(rollout_id);
  if (rollout == nullptr) {
    return make_status(StatusCode::kNotFound, "no such rollout");
  }
  const Status fences = state.check_fences(*rollout, context);
  if (!fences.ok()) {
    return fences;
  }
  if (rollout->state != RolloutState::kArmed) {
    return make_status(StatusCode::kPreconditionFailed,
                       "a rollout can only be started from armed, it is " +
                           std::string(to_string(rollout->state)));
  }
  if (rollout->cursor >= rollout->stages.size()) {
    return make_status(StatusCode::kPreconditionFailed, "the rollout has no stage to start");
  }
  for (const StageRuntime& stage : rollout->stages) {
    if (!stage.cohort.frozen) {
      return make_status(StatusCode::kImmutable,
                         "stage " + orchestrator_detail::short_hex(stage.id) +
                             " has no frozen cohort; the rollout was not correctly armed");
    }
  }
  const Timestamp now = state.clock->wall_now();
  const Status status =
      state.transition(*rollout, RolloutState::kRunning, "started by operator", now, nullptr);
  if (!status.ok()) {
    return status;
  }
  if (detail != nullptr) {
    *detail = "rollout " + orchestrator_detail::short_hex(rollout->id) + " running";
  }
  return Status::success();
}

Status Orchestrator::pause(const RolloutId& rollout_id, const CommandContext& context,
                           std::string reason, std::string* detail) {
  Impl& state = *impl_;
  const Status authority = state.check_authority(context);
  if (!authority.ok()) {
    return authority;
  }
  Rollout* rollout = state.mutable_rollout(rollout_id);
  if (rollout == nullptr) {
    return make_status(StatusCode::kNotFound, "no such rollout");
  }
  const Status fences = state.check_fences(*rollout, context);
  if (!fences.ok()) {
    return fences;
  }
  if (!can_pause(rollout->state)) {
    return make_status(StatusCode::kPreconditionFailed,
                       "a rollout in state " + std::string(to_string(rollout->state)) +
                           " cannot be paused");
  }
  const Timestamp now = state.clock->wall_now();
  const RolloutState previous = rollout->state;
  rollout->resume_state = previous;

  DecisionBuilder builder(DecisionKind::kPauseRollout, rollout->id, rollout->generation,
                          rollout->revision, state.controller_epoch, state.incarnation);
  builder.authority(context.authority);
  builder.input("previous_state", to_string(previous));
  builder.input("reason", reason);
  const StageRuntime* stage = rollout->current_stage();
  const InFlightPolicy in_flight =
      stage != nullptr ? stage->policy.pause_in_flight : InFlightPolicy::kLetFinish;
  builder.input("in_flight_policy", to_string(in_flight));
  builder.select("rollout paused");
  builder.rationale("no further work will be dispatched while the rollout is paused; attempts "
                    "already dispatched are " +
                    std::string(in_flight == InFlightPolicy::kLetFinish ? "allowed to finish"
                                                                        : "cancelled"));
  state.commit_decision(*rollout, builder.finish(state.ids, now, state.limits), now);

  Status status = state.transition(*rollout, RolloutState::kPaused,
                                   reason.empty() ? "paused by operator" : reason, now, nullptr);
  if (!status.ok()) {
    return status;
  }
  if (in_flight == InFlightPolicy::kCancel && stage != nullptr) {
    StageRuntime* mutable_stage = rollout->find_stage(stage->id);
    if (mutable_stage != nullptr) {
      TickReport report;
      status = state.cancel_in_flight(*rollout, *mutable_stage, now, report);
      if (!status.ok()) {
        return status;
      }
    }
  }
  if (detail != nullptr) {
    *detail = "rollout " + orchestrator_detail::short_hex(rollout->id) +
              " paused; new dispatch is stopped";
  }
  return Status::success();
}

Status Orchestrator::resume(const RolloutId& rollout_id, const CommandContext& context,
                            std::string* detail) {
  Impl& state = *impl_;
  const Status authority = state.check_authority(context);
  if (!authority.ok()) {
    return authority;
  }
  Rollout* rollout = state.mutable_rollout(rollout_id);
  if (rollout == nullptr) {
    return make_status(StatusCode::kNotFound, "no such rollout");
  }
  const Status fences = state.check_fences(*rollout, context);
  if (!fences.ok()) {
    return fences;
  }
  if (rollout->state != RolloutState::kPaused) {
    return make_status(StatusCode::kPreconditionFailed,
                       "a rollout can only be resumed from paused, it is " +
                           std::string(to_string(rollout->state)));
  }
  const RolloutState target = rollout->resume_state;
  if (!legal_transition(RolloutState::kPaused, target)) {
    return make_status(StatusCode::kConflict,
                       "the recorded resume state " + std::string(to_string(target)) +
                           " is not reachable from paused");
  }
  const Timestamp now = state.clock->wall_now();
  DecisionBuilder builder(DecisionKind::kResumeRollout, rollout->id, rollout->generation,
                          rollout->revision, state.controller_epoch, state.incarnation);
  builder.authority(context.authority);
  builder.input("resume_state", to_string(target));
  builder.select("rollout resumed");
  builder.rationale("the rollout returns to the exact state it was paused in");
  state.commit_decision(*rollout, builder.finish(state.ids, now, state.limits), now);

  const Status status = state.transition(*rollout, target, "resumed by operator", now, nullptr);
  if (!status.ok()) {
    return status;
  }
  if (detail != nullptr) {
    *detail = "rollout " + orchestrator_detail::short_hex(rollout->id) + " resumed into " +
              std::string(to_string(target));
  }
  return Status::success();
}

Status Orchestrator::abort(const RolloutId& rollout_id, const CommandContext& context,
                           std::string reason, std::string* detail) {
  Impl& state = *impl_;
  const Status authority = state.check_authority(context);
  if (!authority.ok()) {
    return authority;
  }
  Rollout* rollout = state.mutable_rollout(rollout_id);
  if (rollout == nullptr) {
    return make_status(StatusCode::kNotFound, "no such rollout");
  }
  const Status fences = state.check_fences(*rollout, context);
  if (!fences.ok()) {
    return fences;
  }
  if (is_terminal(rollout->state) || rollout->state == RolloutState::kAborting ||
      rollout->state == RolloutState::kRollingBack) {
    return make_status(StatusCode::kPreconditionFailed,
                       "a rollout in state " + std::string(to_string(rollout->state)) +
                           " cannot be aborted");
  }
  const Timestamp now = state.clock->wall_now();
  const bool compensation_available = orchestrator_detail::rollout_has_compensation(*rollout);

  DecisionBuilder builder(DecisionKind::kAbortRollout, rollout->id, rollout->generation,
                          rollout->revision, state.controller_epoch, state.incarnation);
  builder.authority(context.authority);
  builder.input("reason", reason);
  builder.input("compensation_available", compensation_available ? "yes" : "no");
  builder.select("rollout aborting");
  builder.rationale(
      compensation_available
          ? "future stages are stopped and the plan declares compensation, so rollback begins"
          : "future stages are stopped; the plan declares no compensation for the stages that ran, "
            "so the rollout fails without pretending to undo the change");
  state.commit_decision(*rollout, builder.finish(state.ids, now, state.limits), now);

  Status status = state.transition(*rollout, RolloutState::kAborting,
                                   reason.empty() ? "aborted by operator" : reason, now, nullptr);
  if (!status.ok()) {
    return status;
  }
  TickReport report;
  for (StageRuntime& stage : rollout->stages) {
    if (stage.state == StageState::kPending || stage.state == StageState::kBlocked) {
      const Status skipped = state.stage_transition(*rollout, stage, StageState::kSkipped, now,
                                                    &report);
      if (!skipped.ok()) {
        return skipped;
      }
      continue;
    }
    if (!is_terminal(stage.state)) {
      status = state.cancel_in_flight(*rollout, stage, now, report);
      if (!status.ok()) {
        return status;
      }
      const Status aborted = state.stage_transition(*rollout, stage, StageState::kAborted, now,
                                                    &report);
      if (!aborted.ok()) {
        return aborted;
      }
    }
  }
  if (compensation_available) {
    status = state.begin_rollback(*rollout, "abort requested by operator", now, report);
    if (!status.ok()) {
      return status;
    }
  } else {
    status = state.fail_rollout(*rollout, "aborted with no compensation declared by the plan", now,
                                report);
    if (!status.ok()) {
      return status;
    }
  }
  if (detail != nullptr) {
    *detail = "rollout " + orchestrator_detail::short_hex(rollout->id) + " aborting; " +
              (compensation_available ? "compensation started" : "no compensation is available");
  }
  return Status::success();
}

Status Orchestrator::approve_gate(const RolloutId& rollout_id, const GateId& gate,
                                  const CommandContext& context, std::string* detail) {
  Impl& state = *impl_;
  const Status authority = state.check_authority(context);
  if (!authority.ok()) {
    return authority;
  }
  Rollout* rollout = state.mutable_rollout(rollout_id);
  if (rollout == nullptr) {
    return make_status(StatusCode::kNotFound, "no such rollout");
  }
  const Status fences = state.check_fences(*rollout, context);
  if (!fences.ok()) {
    return fences;
  }
  StageRuntime* stage = nullptr;
  for (StageRuntime& candidate : rollout->stages) {
    if (!candidate.pending_gate.is_nil() && candidate.pending_gate == gate) {
      stage = &candidate;
      break;
    }
  }
  if (stage == nullptr) {
    return make_status(StatusCode::kNotFound,
                       "gate " + gate.to_hex().substr(0, 12) +
                           " is not pending; a gate can only be approved once, and only while it "
                           "is open");
  }
  const Timestamp now = state.clock->wall_now();
  const GateKind kind = stage->pending_gate_kind;

  DecisionBuilder builder(DecisionKind::kOpenGate, rollout->id, rollout->generation, rollout->revision,
                          state.controller_epoch, state.incarnation);
  builder.authority(context.authority);
  builder.stage(stage->id);
  builder.input("gate", gate.to_hex());
  builder.input("gate_kind", to_string(kind));
  builder.input("operator", context.operator_name.empty() ? std::string("operator")
                                                          : context.operator_name);
  builder.select("manual gate opened by operator");
  builder.rationale("the operator explicitly approved the pending " + std::string(to_string(kind)) +
                    " gate; the approval is single use and fenced by generation and revision");
  state.commit_decision(*rollout, builder.finish(state.ids, now, state.limits), now);

  stage->pending_gate = GateId{};
  stage->pending_gate_since = Timestamp{};
  TickReport report;

  Status status = Status::success();
  switch (kind) {
    case GateKind::kStageEntry:
      status = state.begin_stage(*rollout, *state.plan_for(rollout->id), *stage, now, report);
      break;
    case GateKind::kEvidence:
    case GateKind::kAdvance:
      status = state.complete_stage(*rollout, *stage, now, report, true);
      break;
    case GateKind::kHealth:
      status = state.stage_transition(*rollout, *stage, StageState::kRunning, now, &report);
      break;
    case GateKind::kRollback:
      status = state.begin_rollback(*rollout, "operator approved rollback", now, report);
      break;
  }
  if (!status.ok()) {
    return status;
  }
  if (detail != nullptr) {
    *detail = "gate " + gate.to_hex().substr(0, 12) + " approved";
  }
  return Status::success();
}

Status Orchestrator::retire(const RolloutId& rollout_id, const CommandContext& context,
                            std::string* detail) {
  Impl& state = *impl_;
  const Status authority = state.check_authority(context);
  if (!authority.ok()) {
    return authority;
  }
  Rollout* rollout = state.mutable_rollout(rollout_id);
  if (rollout == nullptr) {
    return make_status(StatusCode::kNotFound, "no such rollout");
  }
  const Status fences = state.check_fences(*rollout, context);
  if (!fences.ok()) {
    return fences;
  }
  if (rollout->state != RolloutState::kFailed && rollout->state != RolloutState::kCompleted) {
    return make_status(StatusCode::kPreconditionFailed,
                       "only a failed or completed rollout can be retired; this one is " +
                           std::string(to_string(rollout->state)));
  }
  const Timestamp now = state.clock->wall_now();
  const Status status = state.transition(*rollout, RolloutState::kRetired, "retired by operator", now,
                                         nullptr);
  if (!status.ok()) {
    return status;
  }
  if (state.journal != nullptr) {
    ByteWriter writer;
    writer.raw(rollout->id.span());
    const Status retired =
        state.append_journal(JournalRecordType::kRolloutRetired, now, writer.span());
    if (!retired.ok()) {
      return retired;
    }
  }
  if (detail != nullptr) {
    *detail = "rollout " + orchestrator_detail::short_hex(rollout->id) + " retired";
  }
  return Status::success();
}

Status Orchestrator::regenerate(const RolloutId& rollout_id, const CommandContext& context,
                                std::string* detail) {
  Impl& state = *impl_;
  const Status authority = state.check_authority(context);
  if (!authority.ok()) {
    return authority;
  }
  Rollout* rollout = state.mutable_rollout(rollout_id);
  if (rollout == nullptr) {
    return make_status(StatusCode::kNotFound, "no such rollout");
  }
  const Status fences = state.check_fences(*rollout, context);
  if (!fences.ok()) {
    return fences;
  }
  if (rollout->state != RolloutState::kFailed && rollout->state != RolloutState::kCompleted) {
    return make_status(StatusCode::kPreconditionFailed,
                       "a new generation can only be created from a failed or completed rollout; "
                       "this one is " +
                           std::string(to_string(rollout->state)));
  }
  const std::size_t index = state.rollout_index(rollout->id);
  if (index >= state.plans.size()) {
    return make_status(StatusCode::kInternal, "rollout has no stored plan");
  }
  const ChangePlan& plan = *state.plans[index];
  const Timestamp now = state.clock->wall_now();

  // The generation counter is bumped first: every derived identifier, every
  // cohort digest and every idempotency key changes with it, so nothing from
  // the previous generation can be replayed into this one.
  rollout->generation_counter = GenerationCounter{value_of(rollout->generation_counter) + 1};
  rollout->generation = state.ids.next<GenerationId>(IdDomain::kGeneration);
  rollout->cursor = 0;
  rollout->outstanding_attempts = 0;
  rollout->stages.clear();
  for (const StageId& stage_id : plan.execution_order()) {
    const StageSpec* spec = plan.find_stage(stage_id);
    if (spec == nullptr) {
      return make_status(StatusCode::kInternal, "plan order names a stage the plan does not hold");
    }
    StageRuntime stage;
    stage.id = spec->id;
    stage.state = StageState::kPending;
    stage.ordinal = spec->ordinal;
    stage.predecessors = spec->predecessors;
    stage.policy = spec->policy;
    stage.evidence_rule = spec->evidence_rule;
    stage.actions = spec->actions;
    auto materialize = materialize_cohort(spec->selector, state.inventory, spec->id,
                                          rollout->generation, state.limits, now);
    if (!materialize.ok()) {
      return materialize.status();
    }
    if (!materialize.value().truncated.empty()) {
      return make_status(StatusCode::kLimitExceeded,
                         "regenerating stage '" + spec->name +
                             "' would truncate its cohort; refusing to run a partial cohort");
    }
    stage.cohort = materialize.value().membership;
    stage.cohort.id = state.ids.next<CohortId>(IdDomain::kCohort);
    for (const TargetId& member : stage.cohort.members) {
      TargetRuntime target;
      target.target = member;
      stage.targets.push_back(std::move(target));
    }
    rollout->stages.push_back(std::move(stage));
  }
  for (std::size_t stage_index = 0; stage_index < rollout->stages.size(); ++stage_index) {
    StageRuntime& stage = rollout->stages[stage_index];
    for (const StageId& predecessor : stage.predecessors) {
      StageRuntime* predecessor_stage = rollout->find_stage(predecessor);
      if (predecessor_stage != nullptr) {
        predecessor_stage->successors.push_back(stage.id);
      }
    }
  }

  DecisionBuilder builder(DecisionKind::kArmRollout, rollout->id, rollout->generation,
                          rollout->revision, state.controller_epoch, state.incarnation);
  builder.authority(context.authority);
  builder.input_u64("new_generation", value_of(rollout->generation_counter));
  builder.input_digest("inventory", state.inventory.inventory_digest());
  for (const StageRuntime& stage : rollout->stages) {
    builder.input("stage", orchestrator_detail::short_hex(stage.id));
    builder.input_u64("members", static_cast<std::uint64_t>(stage.cohort.size()));
    builder.input_digest("membership", stage.cohort.membership_digest);
  }
  builder.select("new rollout generation created");
  builder.rationale("membership is immutable within a generation, so refreshing it required "
                    "creating a new one; every stage cohort was re-materialised against the "
                    "current inventory");
  state.commit_decision(*rollout, builder.finish(state.ids, now, state.limits), now);

  rollout->revision = next(rollout->revision);
  rollout->state_reason = "regenerated into generation " +
                          std::to_string(value_of(rollout->generation_counter));
  rollout->completed_at = Timestamp{};
  rollout->state = RolloutState::kValidated;
  rollout->resume_state = RolloutState::kValidated;
  rollout->updated_at = now;
  const Status status = state.persist_rollout(*rollout, now);
  if (!status.ok()) {
    return status;
  }
  if (detail != nullptr) {
    *detail = "rollout " + orchestrator_detail::short_hex(rollout->id) + " regenerated as " +
              rollout->generation.to_hex().substr(0, 12);
  }
  return Status::success();
}

Status Orchestrator::set_inventory(const TargetInventory& inventory) {
  Impl& state = *impl_;
  for (const auto& rollout : state.rollouts) {
    const bool mutable_state =
        !is_terminal(rollout->state) && rollout->state != RolloutState::kRetired;
    if (!mutable_state) {
      continue;
    }
    for (const StageRuntime& stage : rollout->stages) {
      if (!stage.cohort.frozen) {
        continue;
      }
      const Digest256 current = state.inventory.inventory_digest();
      if (digest_equal(current, stage.cohort.inventory_digest)) {
        continue;
      }
      // A frozen cohort keeps the membership it was armed with. The inventory
      // may legitimately move on; what may not happen is a frozen cohort
      // silently changing underneath a running rollout.
      continue;
    }
  }
  state.inventory = inventory;
  return Status::success();
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

TickReport Orchestrator::tick() {
  TickReport report;
  Impl& state = *impl_;
  if (state.shutting_down || state.storage_failed) {
    return report;
  }
  const Timestamp now = state.clock->wall_now();

  if (state.execution != nullptr) {
    // Adapters deliver evidence by calling back into on_evidence, which only
    // queues it. Nothing the adapter does can re-enter the state machine.
    state.execution->poll(*this);
  }
  state.drain_pending(report, now);

  for (auto& rollout : state.rollouts) {
    const ChangePlan* plan = state.plan_for(rollout->id);
    if (plan == nullptr) {
      continue;
    }
    state.advance(*rollout, *plan, now, report);
  }
  const Status compacted = state.maybe_compact(now);
  if (!compacted.ok()) {
    state.storage_failed = true;
    state.storage_status = compacted;
  }
  return report;
}

void Orchestrator::note_shutdown() noexcept { impl_->shutting_down = true; }

const Status& Orchestrator::storage_status() const noexcept { return impl_->storage_status; }

void Orchestrator::Impl::drain_pending(TickReport& report, Timestamp now) {
  std::vector<EvidenceRecord> batch;
  batch.swap(pending_evidence);
  for (const EvidenceRecord& record : batch) {
    if (evidence_class(record.kind) == EvidenceClass::kTelemetry) {
      // Telemetry describes a target, not a change, so it is routed by target
      // across every rollout that holds it instead of being matched to an attempt.
      apply_telemetry(record, now);
      continue;
    }
    const Rollout* rollout = nullptr;
    const ChangePlan* plan = nullptr;
    const AdmissionResult admission = admit(record, now, &rollout, &plan);
    if (admission.accepted()) {
      ++counters.evidence_accepted;
      Rollout* mutable_value = mutable_rollout(record.rollout);
      if (mutable_value != nullptr && plan != nullptr) {
        const Status status = apply_evidence(*mutable_value, *plan, record, now, report);
        if (!status.ok()) {
          storage_failed = true;
          storage_status = status;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Evidence admission
// ---------------------------------------------------------------------------

AdmissionResult Orchestrator::Impl::admit(const EvidenceRecord& record, Timestamp now,
                                          const Rollout** rollout_out, const ChangePlan** plan_out) {
  AdmissionResult result;
  if (rollout_out != nullptr) {
    *rollout_out = nullptr;
  }
  if (plan_out != nullptr) {
    *plan_out = nullptr;
  }
  const Status valid = record.validate(limits);
  if (!valid.ok()) {
    ++counters.evidence_rejected;
    result.disposition = EvidenceAdmission::kMalformed;
    result.reason = valid.message();
    return result;
  }
  Rollout* rollout = mutable_rollout(record.rollout);
  if (rollout == nullptr) {
    ++counters.evidence_rejected;
    result.disposition = EvidenceAdmission::kUnknownAttempt;
    result.reason = "no rollout " + orchestrator_detail::short_hex(record.rollout) +
                    " is known to this controller";
    return result;
  }
  const ChangePlan* plan = plan_for(rollout->id);
  if (plan == nullptr) {
    ++counters.evidence_rejected;
    result.disposition = EvidenceAdmission::kMalformed;
    result.reason = "the rollout has no stored plan";
    return result;
  }
  if (rollout_out != nullptr) {
    *rollout_out = rollout;
  }
  if (plan_out != nullptr) {
    *plan_out = plan;
  }
  if (!(record.generation == rollout->generation)) {
    ++counters.evidence_stale_generation;
    result.disposition = EvidenceAdmission::kStaleGeneration;
    result.reason = "evidence names generation " + record.generation.to_hex().substr(0, 12) +
                    " but the rollout is at " + rollout->generation.to_hex().substr(0, 12);
    return result;
  }
  StageRuntime* stage = rollout->find_stage(record.stage);
  if (stage == nullptr) {
    ++counters.evidence_rejected;
    result.disposition = EvidenceAdmission::kMalformed;
    result.reason = "evidence names a stage the rollout does not hold";
    return result;
  }
  TargetRuntime* target = stage->find_target(record.target);
  if (target == nullptr) {
    ++counters.evidence_rejected;
    result.disposition = EvidenceAdmission::kMalformed;
    result.reason = "evidence names a target outside the frozen cohort of its stage";
    return result;
  }
  const auto attempt = attempts.find(record.attempt);
  if (attempt == attempts.end()) {
    ++counters.evidence_rejected;
    result.disposition = EvidenceAdmission::kUnknownAttempt;
    result.reason = "attempt " + orchestrator_detail::short_hex(record.attempt) +
                    " is not outstanding on this controller";
    return result;
  }

  const EvidenceClass klass = evidence_class(record.kind);
  const bool from_current_incarnation = record.incarnation == incarnation;
  if (!from_current_incarnation) {
    // A superseded incarnation may still have work in flight, and an effect
    // that really happened must not be discarded. What a superseded incarnation
    // may NOT do is describe the present: telemetry and acceptances from it are
    // refused, because they are claims about a world that has moved on.
    if (klass == EvidenceClass::kTelemetry || klass == EvidenceClass::kAcceptance ||
        klass == EvidenceClass::kCancellation) {
      ++counters.evidence_stale_incarnation;
      result.disposition = EvidenceAdmission::kStaleIncarnation;
      result.reason = "evidence from controller incarnation " +
                      record.incarnation.to_hex().substr(0, 12) +
                      " cannot describe current state after this controller took over";
      return result;
    }
  }

  const Attempt& live = attempt->second;
  if (!(record.attempt_epoch == live.epoch)) {
    ++counters.evidence_stale_attempt;
    result.disposition = EvidenceAdmission::kStaleAttempt;
    result.reason = "evidence names attempt epoch " +
                    std::to_string(value_of(record.attempt_epoch)) +
                    " but the attempt is at epoch " + std::to_string(value_of(live.epoch));
    return result;
  }
  if (value_of(record.sequence) == value_of(live.highest_sequence)) {
    ++counters.evidence_duplicate;
    result.disposition = EvidenceAdmission::kDuplicate;
    result.reason = "sequence " + std::to_string(value_of(record.sequence)) +
                    " has already been applied to this attempt";
    return result;
  }
  // Ordering is classified before terminal-state fencing: a re-sent record is a
  // duplicate, an older record is out of order, and only a genuinely newer
  // report for an attempt that already finished is late.
  if (value_of(record.sequence) < value_of(live.highest_sequence)) {
    ++counters.evidence_out_of_order;
    result.disposition = EvidenceAdmission::kOutOfOrder;
    result.reason = "sequence " + std::to_string(value_of(record.sequence)) +
                    " arrives after " + std::to_string(value_of(live.highest_sequence)) +
                    " and describes an older observation";
    return result;
  }
  if (live.finished()) {
    ++counters.evidence_stale_attempt;
    result.disposition = EvidenceAdmission::kStaleAttempt;
    result.reason = "the attempt already reached a terminal state; late evidence is "
                    "recorded as an observation but cannot resurrect it";
    return result;
  }

  // Freshness. Telemetry is admitted and let through to the gate, which applies
  // the policy's tighter bound; a terminal execution report that is older than
  // the stage's evidence lifetime is refused outright, because a completion
  // that ancient cannot be trusted to describe the change that was dispatched.
  if (klass != EvidenceClass::kTelemetry) {
    const Duration max_age = stage->policy.evidence_max_age.is_zero()
                                 ? Duration::from_millis(limits.default_evidence_max_age_millis)
                                 : stage->policy.evidence_max_age;
    const Freshness freshness =
        evaluate_freshness(record.observed_at, now, max_age,
                           Duration::from_millis(limits.max_future_skew_millis));
    if (!freshness.fresh) {
      ++counters.evidence_rejected;
      result.disposition = EvidenceAdmission::kExpired;
      result.reason = "evidence is unusable: " + freshness.reason;
      return result;
    }
  }

  result.disposition = EvidenceAdmission::kAccepted;
  result.reason = "admitted";
  return result;
}

}  // namespace rollout_fabric
