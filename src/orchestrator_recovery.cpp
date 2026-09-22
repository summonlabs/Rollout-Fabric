// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Restart-safe recovery, reconciliation against downstream execution evidence,
// and the observation API.
//
// The journal is the authority on orchestration state. It is NOT the authority
// on downstream effects: after a restart the runtime asks the execution adapter
// what happened to the attempts it believed were outstanding, and it asks
// before it dispatches anything. That is why a restart cannot redispatch an
// effect that already happened, and why persisted telemetry never becomes fresh
// merely because it could be deserialised.
#include <algorithm>
#include <string>
#include <utility>

#include "orchestrator_impl.hpp"

namespace rollout_fabric {
namespace {

using namespace orchestrator_detail;

}  // namespace

// ---------------------------------------------------------------------------
// Sink
// ---------------------------------------------------------------------------

void Orchestrator::on_evidence(EvidenceRecord record) {
  Impl& state = *impl_;
  if (state.pending_evidence.size() >= state.limits.max_queued_frames_per_connection * 4u) {
    ++state.counters.evidence_rejected;
    return;
  }
  state.pending_evidence.push_back(std::move(record));
}

void Orchestrator::on_adapter_event(AdapterEvent event) {
  Impl& state = *impl_;
  state.note_adapter_event(event);
}

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

const Rollout* Orchestrator::find(const RolloutId& id) const noexcept { return impl_->find(id); }

std::vector<const Rollout*> Orchestrator::rollouts() const {
  std::vector<const Rollout*> values;
  values.reserve(impl_->rollouts.size());
  for (const auto& entry : impl_->rollouts) {
    values.push_back(entry.get());
  }
  return values;
}

const TargetInventory& Orchestrator::inventory() const noexcept { return impl_->inventory; }
const RuntimeLimits& Orchestrator::limits() const noexcept { return impl_->limits; }
const IncarnationId& Orchestrator::incarnation() const noexcept { return impl_->incarnation; }
EpochCounter Orchestrator::controller_epoch() const noexcept { return impl_->controller_epoch; }
const Clock& Orchestrator::clock() const noexcept { return *impl_->clock; }

const ChangePlan* Orchestrator::plan_for(const RolloutId& id) const noexcept {
  return impl_->plan_for(id);
}

const Attempt* Orchestrator::find_attempt(const AttemptId& id) const noexcept {
  const auto found = impl_->attempts.find(id);
  if (found == impl_->attempts.end()) {
    return nullptr;
  }
  return &found->second;
}

std::vector<Attempt> Orchestrator::outstanding_attempts(const RolloutId& id) const {
  std::vector<Attempt> values;
  for (const auto& entry : impl_->attempts) {
    if (entry.second.rollout == id && !entry.second.finished()) {
      values.push_back(entry.second);
    }
  }
  std::sort(values.begin(), values.end(),
            [](const Attempt& lhs, const Attempt& rhs) { return lhs.id < rhs.id; });
  return values;
}

const OrchestratorCounters& Orchestrator::counters() const noexcept { return impl_->counters; }

const std::vector<AdapterEvent>& Orchestrator::recent_adapter_events() const {
  return impl_->adapter_events;
}

Result<StageRuntime> Orchestrator::stage_view(const RolloutId& rollout_id,
                                              const StageId& stage_id) const {
  const Rollout* rollout = impl_->find(rollout_id);
  if (rollout == nullptr) {
    return make_status(StatusCode::kNotFound, "no such rollout");
  }
  const StageRuntime* stage = rollout->find_stage(stage_id);
  if (stage == nullptr) {
    return make_status(StatusCode::kNotFound, "no such stage in this rollout");
  }
  return *stage;
}

Result<CohortMembership> Orchestrator::cohort_view(const RolloutId& rollout_id,
                                                   const StageId& stage_id) const {
  auto stage = stage_view(rollout_id, stage_id);
  if (!stage.ok()) {
    return stage.status();
  }
  return stage.value().cohort;
}

std::vector<Decision> Orchestrator::recent_decisions(const RolloutId& rollout_id,
                                                     std::size_t count) const {
  const Rollout* rollout = impl_->find(rollout_id);
  if (rollout == nullptr) {
    return {};
  }
  const std::size_t available = rollout->decisions.size();
  const std::size_t wanted = std::min(count, available);
  return std::vector<Decision>(rollout->decisions.end() - static_cast<std::ptrdiff_t>(wanted),
                               rollout->decisions.end());
}

Result<std::string> Orchestrator::explain(const RolloutId& rollout_id,
                                          const std::optional<DecisionId>& decision) const {
  const Rollout* rollout = impl_->find(rollout_id);
  if (rollout == nullptr) {
    return make_status(StatusCode::kNotFound, "no such rollout");
  }
  if (decision.has_value()) {
    for (const Decision& entry : rollout->decisions) {
      if (entry.id == *decision) {
        return entry.render();
      }
    }
    return make_status(StatusCode::kNotFound,
                       "decision " + decision->to_hex().substr(0, 12) +
                           " is not in the retained history of this rollout");
  }
  if (rollout->decisions.empty()) {
    return make_status(StatusCode::kNotFound,
                       "the rollout has recorded no decision that this controller still retains");
  }
  std::string text;
  text.append("rollout ").append(short_hex(rollout->id)).append(" state=");
  text.append(to_string(rollout->state)).append(" generation=");
  text.append(rollout->generation.to_hex().substr(0, 12)).append(" revision=");
  text.append(std::to_string(value_of(rollout->revision))).append("\n");
  text.append("reason: ").append(rollout->state_reason).append("\n\n");
  const Decision& latest = rollout->decisions.back();
  text.append(latest.render());
  return text;
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

Status Orchestrator::restore(Rollout rollout, const ChangePlan& plan, const RecoveryReport& report) {
  Impl& state = *impl_;
  (void)report;
  if (rollout.id.is_nil()) {
    return make_status(StatusCode::kCorrupt, "the recovered rollout has a nil identifier");
  }
  ChangePlan working = plan;
  const Status plan_status = working.validate(state.limits);
  if (!plan_status.ok()) {
    return make_status(plan_status.code(), "the stored plan is not valid: " + plan_status.message());
  }
  if (!digest_equal(rollout.plan_digest, working.plan_digest())) {
    return make_status(StatusCode::kCorrupt,
                       "the stored plan does not match the digest the rollout was created with");
  }
  if (!(rollout.plan_id == working.id)) {
    return make_status(StatusCode::kCorrupt, "the stored plan identifier does not match the rollout");
  }
  for (std::size_t index = 0; index < rollout.stages.size(); ++index) {
    const StageRuntime& stage = rollout.stages[index];
    if (working.find_stage(stage.id) == nullptr) {
      return make_status(StatusCode::kCorrupt,
                         "the recovered rollout holds a stage the stored plan does not declare");
    }
    if (!stage.cohort.frozen && !is_terminal(rollout.state)) {
      return make_status(StatusCode::kCorrupt,
                         "a recovered non-terminal rollout has an unfrozen cohort; refusing to "
                         "adopt state whose membership could still change");
    }
  }

  const std::size_t existing = state.rollout_index(rollout.id);
  if (existing != state.rollouts.size()) {
    // A later snapshot of the same rollout supersedes an earlier one; an
    // earlier snapshot never overwrites a later one.
    if (!(state.rollouts[existing]->revision < rollout.revision)) {
      return make_status(StatusCode::kStaleRevision,
                         "the recovered snapshot is not newer than the state already held");
    }
    state.rollouts.erase(state.rollouts.begin() + static_cast<std::ptrdiff_t>(existing));
    state.plans.erase(state.plans.begin() + static_cast<std::ptrdiff_t>(existing));
  }

  // The incarnation that dispatched any outstanding attempt is the one the
  // snapshot was written under. It is recorded on the reconstructed attempts so
  // that evidence arriving from it is recognised for what it is.
  const IncarnationId previous_incarnation = rollout.incarnation;
  rollout.incarnation = state.incarnation;
  rollout.controller_epoch = state.controller_epoch;
  if (is_terminal(rollout.state)) {
    rollout.state_reason = rollout.state_reason.empty() ? "recovered in a terminal state"
                                                        : rollout.state_reason;
  } else {
    rollout.state_reason = "recovered by controller incarnation " +
                           state.incarnation.to_hex().substr(0, 12) + "; " + rollout.state_reason;
  }

  std::uint32_t outstanding = 0;
  for (StageRuntime& stage : rollout.stages) {
    for (TargetRuntime& target : stage.targets) {
      if (target.current_attempt.is_nil()) {
        continue;
      }
      if (target.state != TargetState::kDispatched && target.state != TargetState::kUnknown &&
          target.state != TargetState::kRollingBack) {
        target.current_attempt = AttemptId{};
        continue;
      }
      const ActionSpec* action = nullptr;
      if (target.state == TargetState::kRollingBack) {
        action = compensation_action(stage, target.rollback_step);
      } else {
        action = primary_action(stage);
      }
      if (action == nullptr) {
        target.current_attempt = AttemptId{};
        continue;
      }
      Attempt attempt;
      attempt.id = target.current_attempt;
      attempt.rollout = rollout.id;
      attempt.generation = rollout.generation;
      attempt.stage = stage.id;
      attempt.cohort = stage.cohort.id;
      attempt.target = target.target;
      attempt.action = action->id;
      attempt.epoch = target.epoch == AttemptEpoch{0} ? AttemptEpoch{1} : target.epoch;
      attempt.attempt_number = target.attempts_created == 0 ? 1 : target.attempts_created;
      attempt.incarnation = previous_incarnation;
      attempt.action_digest = action->action_digest();
      attempt.artifact_digest = action->artifact_digest;
      attempt.idempotency_key = derive_idempotency_key(rollout.id, rollout.generation, stage.id,
                                                       target.target, action->id);
      attempt.created_at = target.dispatched_at;
      attempt.dispatched_at = target.dispatched_at;
      attempt.deadline = target.dispatched_at + stage.policy.attempt_deadline;
      attempt.state = TargetState::kDispatched;
      state.note_attempt(attempt);
      state.note_idempotency_key(attempt.idempotency_key);
      ++outstanding;
    }
  }
  rollout.outstanding_attempts = outstanding;
  ++state.counters.journal_appends;

  state.rollouts.push_back(std::make_unique<Rollout>(std::move(rollout)));
  state.plans.push_back(std::make_unique<ChangePlan>(std::move(working)));
  return Status::success();
}

// ---------------------------------------------------------------------------
// Reconciliation
// ---------------------------------------------------------------------------

Status Orchestrator::reconcile_outstanding(const RolloutId& rollout_id, std::string* detail) {
  Impl& state = *impl_;
  Rollout* rollout = state.mutable_rollout(rollout_id);
  if (rollout == nullptr) {
    return make_status(StatusCode::kNotFound, "no such rollout");
  }
  const ChangePlan* plan = state.plan_for(rollout_id);
  if (plan == nullptr) {
    return make_status(StatusCode::kInternal, "the rollout has no stored plan");
  }
  if (state.execution == nullptr) {
    return make_status(StatusCode::kUnavailable, "no execution adapter is attached");
  }
  std::vector<AttemptFence> fences;
  std::vector<AttemptId> ids;
  for (const auto& entry : state.attempts) {
    if (entry.second.rollout == rollout_id && !entry.second.finished()) {
      fences.push_back(entry.second.fence());
      ids.push_back(entry.first);
    }
  }
  if (fences.empty()) {
    if (detail != nullptr) {
      *detail = "no outstanding attempt needs reconciliation";
    }
    return Status::success();
  }
  if (fences.size() > state.limits.max_concurrent_attempts) {
    return make_status(StatusCode::kLimitExceeded,
                       "more outstanding attempts than max_concurrent_attempts; refusing to issue an "
                       "unbounded reconcile request");
  }

  const ReconcileResult result = state.execution->reconcile(fences);
  const Timestamp now = state.clock->wall_now();
  if (!result.ok) {
    return make_status(StatusCode::kUnavailable,
                       "the execution adapter could not answer the reconciliation: " + result.detail);
  }

  std::uint32_t resolved = 0;
  std::uint32_t released = 0;
  for (const ReconcileEntry& entry : result.entries) {
    const auto found = state.attempts.find(entry.fence.attempt);
    if (found == state.attempts.end()) {
      continue;
    }
    Attempt& attempt = found->second;
    StageRuntime* stage = rollout->find_stage(attempt.stage);
    if (stage == nullptr) {
      continue;
    }
    TargetRuntime* target = stage->find_target(attempt.target);
    if (target == nullptr) {
      continue;
    }
    switch (entry.disposition) {
      case ReconcileDisposition::kCompleted:
      case ReconcileDisposition::kFailed: {
        EvidenceRecord record;
        record.id = state.ids.next<EvidenceId>(IdDomain::kEvidence);
        record.rollout = attempt.rollout;
        record.generation = attempt.generation;
        record.stage = attempt.stage;
        record.cohort = attempt.cohort;
        record.target = attempt.target;
        record.attempt = attempt.id;
        record.attempt_epoch = attempt.epoch;
        record.incarnation = attempt.incarnation;
        record.kind = entry.disposition == ReconcileDisposition::kCompleted
                          ? EvidenceKind::kExecutionCompleted
                          : EvidenceKind::kExecutionFailed;
        record.outcome = entry.disposition == ReconcileDisposition::kCompleted
                             ? EvidenceOutcome::kSucceeded
                             : EvidenceOutcome::kFailed;
        record.authority = EvidenceAuthority::kExecutionAdapterAttested;
        record.origin = EvidenceOrigin::kReconciledFromAdapter;
        record.sequence = Sequence{value_of(attempt.highest_sequence) + 1};
        record.observed_at = now;
        record.recorded_at = now;
        record.payload_digest = sha256(entry.detail);
        TickReport report;
        const AdmissionResult admission = state.admit(record, now, nullptr, nullptr);
        if (admission.accepted()) {
          const Status status = state.apply_evidence(*rollout, *plan, record, now, report);
          if (!status.ok()) {
            return status;
          }
          ++resolved;
        }
        break;
      }
      case ReconcileDisposition::kNotStarted: {
        // The executor never began the effect. The attempt is released without
        // being completed, and the target becomes dispatchable again under the
        // same idempotency key.
        if (target->state == TargetState::kDispatched || target->state == TargetState::kUnknown) {
          target->state = TargetState::kPending;
        }
        target->current_attempt = AttemptId{};
        attempt.completed = true;
        attempt.state = TargetState::kCancelled;
        attempt.outcome = EvidenceOutcome::kInconclusive;
        if (rollout->outstanding_attempts > 0) {
          --rollout->outstanding_attempts;
        }
        ++released;
        break;
      }
      case ReconcileDisposition::kInProgress:
        break;
      case ReconcileDisposition::kUnknown:
        if (target->state == TargetState::kDispatched) {
          target->state = TargetState::kUnknown;
        }
        attempt.state = TargetState::kUnknown;
        break;
    }
  }

  DecisionBuilder builder(DecisionKind::kReconcile, rollout->id, rollout->generation, rollout->revision,
                          state.controller_epoch, state.incarnation);
  builder.input_u64("asked", static_cast<std::uint64_t>(fences.size()));
  builder.input_u64("resolved", resolved);
  builder.input_u64("released", released);
  builder.input_u64("answered", static_cast<std::uint64_t>(result.entries.size()));
  builder.select("reconciled outstanding attempts against the execution adapter");
  builder.reject("dispatch again immediately",
                 "the adapter is asked first; only attempts it never started are redispatched");
  builder.rationale(std::to_string(resolved) + " attempt(s) resolved from downstream evidence, " +
                    std::to_string(released) +
                    " released because the executor never started them; the remainder stay "
                    "outstanding under the same idempotency keys");
  state.commit_decision(*rollout, builder.finish(state.ids, now, state.limits), now);
  const Status persisted = state.persist_rollout(*rollout, now);
  if (!persisted.ok()) {
    return persisted;
  }
  if (detail != nullptr) {
    *detail = "reconciled " + std::to_string(fences.size()) + " outstanding attempt(s): " +
              std::to_string(resolved) + " resolved, " + std::to_string(released) + " released";
  }
  return Status::success();
}

}  // namespace rollout_fabric
