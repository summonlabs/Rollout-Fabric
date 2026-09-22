// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The advancing state machine: stage entry, dispatch, gating, failure,
// compensation and the application of admitted evidence.
//
// Every transition in this file is fenced. A stage may only be entered after
// its predecessors succeeded, a target may only be dispatched under a current
// generation with a fresh epoch, and a report may only move a target that is
// still outstanding under the epoch it names.
#include <algorithm>
#include <string>
#include <utility>

#include "orchestrator_impl.hpp"

namespace rollout_fabric {
namespace {

using namespace orchestrator_detail;

// The health view a gate is evaluated against is derived from the per-target
// observations that were actually admitted. Its observation time is the oldest
// one it rests on: a population is only as fresh as its stalest member.
[[nodiscard]] HealthSnapshot snapshot_for(const StageRuntime& stage, const HealthSourceId& source,
                                          bool have_source) {
  HealthSnapshot snapshot;
  if (have_source) {
    snapshot.source = source;
  }
  bool any = false;
  bool all_live = true;
  Timestamp oldest{};
  for (const TargetRuntime& target : stage.targets) {
    if (target.last_healthy_at.is_nil()) {
      continue;
    }
    if (!any || target.last_healthy_at < oldest) {
      oldest = target.last_healthy_at;
    }
    any = true;
    if (!target.last_health_origin_live) {
      all_live = false;
    }
  }
  snapshot.observed_at = oldest;
  snapshot.live = any && all_live;
  for (const TargetRuntime& target : stage.targets) {
    if (target.healthy) {
      ++snapshot.healthy;
    } else if (target.last_healthy_at.is_nil()) {
      ++snapshot.unknown;
    } else {
      ++snapshot.unhealthy;
    }
  }
  ByteWriter writer;
  writer.string("rollout-fabric/health-snapshot/v1");
  writer.raw(snapshot.source.span());
  writer.i64(snapshot.observed_at.unix_nanos());
  writer.boolean(snapshot.live);
  writer.u32(snapshot.healthy);
  writer.u32(snapshot.unhealthy);
  writer.u32(snapshot.unknown);
  snapshot.digest = sha256(writer.span());
  return snapshot;
}

}  // namespace

// ---------------------------------------------------------------------------
// Applying admitted evidence
// ---------------------------------------------------------------------------

Status Orchestrator::Impl::apply_evidence(Rollout& rollout, const ChangePlan& plan,
                                          const EvidenceRecord& record, Timestamp now,
                                          TickReport& report) {
  (void)plan;
  StageRuntime* stage = rollout.find_stage(record.stage);
  if (stage == nullptr) {
    return Status::success();
  }
  TargetRuntime* target = stage->find_target(record.target);
  if (target == nullptr) {
    return Status::success();
  }
  auto found = attempts.find(record.attempt);
  if (found == attempts.end()) {
    return Status::success();
  }
  Attempt& attempt = found->second;
  attempt.highest_sequence = record.sequence;
  attempt.last_evidence_at = record.recorded_at;
  ++attempt.accepted_evidence;

  switch (evidence_class(record.kind)) {
    case EvidenceClass::kTelemetry:
      // Routed by target before admission; a telemetry record never reaches
      // this path.
      break;
    case EvidenceClass::kAcceptance:
      // Acceptance proves only that the executor took the work; it never
      // completes anything.
      break;
    case EvidenceClass::kCancellation:
      // The executor confirmed it stopped. The attempt is finished as
      // cancelled and can never publish a success afterwards, because the
      // attempt is already terminal when any later report arrives.
      attempt.completed = true;
      attempt.state = TargetState::kCancelled;
      attempt.outcome = EvidenceOutcome::kInconclusive;
      if (rollout.outstanding_attempts > 0) {
        --rollout.outstanding_attempts;
      }
      if (target->state == TargetState::kDispatched || target->state == TargetState::kUnknown) {
        target->state = TargetState::kCancelled;
      }
      target->current_attempt = AttemptId{};
      target->last_reason = "the executor acknowledged the cancellation";
      ++stage->cancelled_total;
      break;
    case EvidenceClass::kExecution:
      // Only a terminal execution kind finishes an attempt. A progress report
      // proves the executor is alive and nothing more; treating it as a
      // terminal record would fail every attempt that reports progress.
      if (record.kind != EvidenceKind::kExecutionCompleted &&
          record.kind != EvidenceKind::kExecutionFailed) {
        break;
      }
      // Terminal execution evidence. This is the only path that can complete
      // a target, and it is reached only for an attempt that is still
      // outstanding under the epoch it names.
      if (target->state == TargetState::kRollingBack) {
        // The runtime knows what it asked this attempt to do: a target in
        // kRollingBack has a compensation outstanding, so a terminal report for
        // it is compensation evidence whatever kind the executor labelled it.
        apply_compensation(record, rollout, *stage, *target, now, report);
      } else {
        apply_terminal_execution(record, rollout, *stage, *target, attempt, now, report);
      }
      break;
    case EvidenceClass::kCompensation:
      apply_compensation(record, rollout, *stage, *target, now, report);
      break;
    case EvidenceClass::kVerification: {
      target->verification_observed_at = record.observed_at;
      target->verification_passed = record.kind == EvidenceKind::kVerificationPassed;
      target->verification_verifier = record.verifier;
      if (!target->verification_passed) {
        target->last_reason = "verification failed";
      }
      break;
    }
  }
  return persist_rollout(rollout, now);
}

bool Orchestrator::Impl::apply_telemetry(const EvidenceRecord& record, Timestamp now) {
  const Duration max_age = Duration::from_millis(limits.default_evidence_max_age_millis);
  const Freshness freshness = evaluate_freshness(record.observed_at, now, max_age,
                                                 Duration::from_millis(limits.max_future_skew_millis));
  if (!freshness.fresh) {
    ++counters.evidence_rejected;
    return false;
  }
  bool matched = false;
  for (auto& rollout : rollouts) {
    bool touched = false;
    for (StageRuntime& stage : rollout->stages) {
      TargetRuntime* target = stage.find_target(record.target);
      if (target == nullptr) {
        continue;
      }
      apply_health(record, *rollout, *target, now);
      touched = true;
      matched = true;
    }
    if (touched) {
      const Status status = persist_rollout(*rollout, now);
      if (!status.ok()) {
        storage_failed = true;
        storage_status = status;
      }
    }
  }
  if (matched) {
    ++counters.evidence_accepted;
  } else {
    ++counters.evidence_rejected;
  }
  return matched;
}

void Orchestrator::Impl::apply_health(const EvidenceRecord& record, Rollout& rollout,
                                      TargetRuntime& target, Timestamp now) {
  (void)rollout;
  const Duration max_age = Duration::from_millis(limits.default_evidence_max_age_millis);
  const Freshness freshness = evaluate_freshness(record.observed_at, now, max_age,
                                                 Duration::from_millis(limits.max_future_skew_millis));
  if (!freshness.fresh) {
    // A stale sample is recorded against nothing: it must not refresh the
    // target's apparent health merely because it arrived.
    target.last_reason = "a health sample was discarded: " + freshness.reason;
    return;
  }
  const bool healthy = record.outcome == EvidenceOutcome::kSucceeded;
  target.last_healthy_at = record.observed_at;
  target.last_health_source = record.health_source;
  target.last_health_origin_live = record.origin == EvidenceOrigin::kLive;
  if (healthy) {
    target.healthy = true;
    target.consecutive_healthy += 1;
    if (target.healthy_since.is_nil()) {
      target.healthy_since = record.observed_at;
    }
  } else {
    target.healthy = false;
    target.consecutive_healthy = 0;
    target.healthy_since = Timestamp{};
  }
}

void Orchestrator::Impl::retry_or_exhaust(Rollout& rollout, StageRuntime& stage, TargetRuntime& target,
                                          Timestamp now, TickReport& report) {
  if (target.attempts_created < stage.policy.max_attempts_per_target) {
    target.state = TargetState::kPending;
    target.current_attempt = AttemptId{};
    DecisionBuilder builder(DecisionKind::kRetryTarget, rollout.id, rollout.generation, rollout.revision,
                            controller_epoch, incarnation);
    builder.stage(stage.id);
    builder.target(target.target);
    builder.input_u64("attempts_created", target.attempts_created);
    builder.input_u64("max_attempts", stage.policy.max_attempts_per_target);
    builder.input("reason", target.last_reason);
    builder.select("target eligible for another attempt");
    builder.reject("exhaust target", "the attempt budget for this stage is not spent yet");
    builder.rationale("the target failed, and the stage allows " +
                      std::to_string(stage.policy.max_attempts_per_target) +
                      " attempt(s) per target; the retry reuses the same idempotency key so an "
                      "executor that already performed the effect will report it instead of "
                      "performing it again");
    commit_decision(rollout, builder.finish(ids, now, limits), now);
    ++report.decisions;
    return;
  }
  if (legal_transition(target.state, TargetState::kExhausted)) {
    target.state = TargetState::kExhausted;
  }
  DecisionBuilder builder(DecisionKind::kFailStage, rollout.id, rollout.generation, rollout.revision,
                          controller_epoch, incarnation);
  builder.stage(stage.id);
  builder.target(target.target);
  builder.input_u64("attempts_created", target.attempts_created);
  builder.input("reason", target.last_reason);
  builder.select("target exhausted its attempt budget");
  builder.rationale("the target failed and every attempt permitted by the stage policy has been "
                    "used; no further dispatch will be made for it in this generation");
  commit_decision(rollout, builder.finish(ids, now, limits), now);
  ++report.decisions;
}

void Orchestrator::Impl::apply_terminal_execution(const EvidenceRecord& record, Rollout& rollout,
                                                  StageRuntime& stage, TargetRuntime& target,
                                                  Attempt& attempt, Timestamp now,
                                                  TickReport& report) {
  const bool was_cancelled = value_of(attempt.cancelled_epoch) != 0;
  attempt.completed = true;
  attempt.last_evidence_at = record.recorded_at;
  attempt.outcome = record.outcome;
  rollout.outstanding_attempts = rollout.outstanding_attempts > 0 ? rollout.outstanding_attempts - 1 : 0;

  if (was_cancelled) {
    // A cancelled operation must never publish success. The effect may well
    // have happened, but this runtime will not treat it as a completed change.
    attempt.state = TargetState::kCancelled;
    target.state = TargetState::kCancelled;
    target.outcome = EvidenceOutcome::kInconclusive;
    target.last_reason = "the attempt was cancelled before it reported; its result is not counted";
    ++stage.cancelled_total;
    return;
  }

  if (!(target.current_attempt == record.attempt)) {
    attempt.state = TargetState::kUnknown;
    return;
  }

  if (record.outcome == EvidenceOutcome::kSucceeded &&
      record.kind == EvidenceKind::kExecutionCompleted) {
    attempt.state = TargetState::kSucceeded;
    target.state = TargetState::kSucceeded;
    target.outcome = EvidenceOutcome::kSucceeded;
    target.completed_at = record.observed_at;
    target.last_terminal_evidence = record.id;
    target.last_reason.clear();
    ++stage.succeeded_total;
    ++report.completed;
    DecisionBuilder builder(DecisionKind::kOpenGate, rollout.id, rollout.generation, rollout.revision,
                            controller_epoch, incarnation);
    builder.stage(stage.id);
    builder.target(target.target);
    builder.attempt(attempt.id);
    builder.input("evidence", orchestrator_detail::short_hex(record.id));
    builder.input("authority", to_string(record.authority));
    builder.input("origin", to_string(record.origin));
    builder.input_u64("sequence", value_of(record.sequence));
    builder.select("target completed");
    builder.rationale("terminal execution evidence from " + std::string(to_string(record.authority)) +
                      " reports that the effect finished successfully; dispatch alone would not "
                      "have been enough");
    commit_decision(rollout, builder.finish(ids, now, limits), now);
    ++report.decisions;
    return;
  }

  attempt.state = TargetState::kFailed;
  target.state = TargetState::kFailed;
  target.outcome = EvidenceOutcome::kFailed;
  target.completed_at = record.observed_at;
  target.last_terminal_evidence = record.id;
  target.last_reason = "terminal execution evidence reported failure";
  ++stage.failed_total;
  retry_or_exhaust(rollout, stage, target, now, report);
}

void Orchestrator::Impl::apply_compensation(const EvidenceRecord& record, Rollout& rollout,
                                            StageRuntime& stage, TargetRuntime& target, Timestamp now,
                                            TickReport& report) {
  rollout.outstanding_attempts = rollout.outstanding_attempts > 0 ? rollout.outstanding_attempts - 1 : 0;
  const auto found = attempts.find(record.attempt);
  if (found != attempts.end()) {
    found->second.completed = true;
    found->second.state = record.outcome == EvidenceOutcome::kSucceeded ? TargetState::kRolledBack
                                                                       : TargetState::kRollbackFailed;
    found->second.outcome = record.outcome;
  }
  // The outcome, not the executor's choice of kind, decides whether the
  // compensating action took effect.
  if (record.outcome == EvidenceOutcome::kSucceeded) {
    ++target.rollback_step;
    if (target.rollback_step >= stage.policy.compensation_actions.size()) {
      target.state = TargetState::kRolledBack;
      target.current_attempt = AttemptId{};
      target.last_reason = "compensated";
      ++stage.rolled_back_total;
    }
    // Otherwise the target stays in kRollingBack and the next compensation step
    // is dispatched on the following tick.
    return;
  }
  target.state = TargetState::kRollbackFailed;
  target.current_attempt = AttemptId{};
  target.last_reason = "compensation failed";
  ++stage.rollback_failed_total;
  DecisionBuilder builder(DecisionKind::kBeginRollback, rollout.id, rollout.generation, rollout.revision,
                          controller_epoch, incarnation);
  builder.stage(stage.id);
  builder.target(target.target);
  builder.attempt(record.attempt);
  builder.input("reason", target.last_reason);
  builder.select("compensation failed for this target");
  builder.rationale("the plan declared compensation for this stage and the compensating action "
                    "reported failure; the target is recorded as rollback_failed rather than as "
                    "restored");
  commit_decision(rollout, builder.finish(ids, now, limits), now);
  ++report.decisions;
}

// ---------------------------------------------------------------------------
// Gating helpers
// ---------------------------------------------------------------------------

void Orchestrator::Impl::open_gate(Rollout& rollout, StageRuntime& stage, GateKind kind,
                                   Timestamp now) {
  stage.pending_gate = ids.next<GateId>(IdDomain::kGate);
  stage.pending_gate_kind = kind;
  stage.pending_gate_since = now;
  const Status status = stage_transition(rollout, stage, StageState::kGated, now, nullptr);
  if (!status.ok()) {
    storage_failed = true;
    storage_status = status;
    return;
  }
  if (legal_transition(rollout.state, RolloutState::kGated)) {
    const Status rollout_status =
        transition(rollout, RolloutState::kGated,
                   "waiting for a manual " + std::string(to_string(kind)) + " gate", now, nullptr);
    if (!rollout_status.ok()) {
      storage_failed = true;
      storage_status = rollout_status;
    }
  }
}

bool Orchestrator::Impl::health_gate_holds(const StageRuntime& stage, Timestamp now) {
  if (!stage.policy.health_gate.enabled) {
    return false;
  }
  const HealthSourceId source = health != nullptr ? health->source_id() : HealthSourceId{};
  const HealthSnapshot snapshot = snapshot_for(stage, source, health != nullptr);
  const GateEvaluation evaluation =
      evaluate_health_gate(stage, stage.policy.health_gate, snapshot, now, limits);
  return !evaluation.satisfied;
}

void Orchestrator::Impl::probe_health(Rollout& rollout, const StageRuntime& stage, Timestamp now) {
  if (health == nullptr || !stage.policy.health_gate.enabled) {
    return;
  }
  const auto last = last_probe_at.find(rollout.id);
  const Duration interval = stage.policy.health_gate.max_age.is_zero()
                                ? Duration::from_millis(100)
                                : Duration::from_millis(stage.policy.health_gate.max_age.millis() / 2);
  if (last != last_probe_at.end() && !(now - last->second > interval)) {
    return;
  }
  last_probe_at[rollout.id] = now;
  HealthProbeRequest request;
  request.issued_at = now;
  request.request_id = ++health_request_id;
  request.targets.reserve(stage.cohort.members.size());
  for (const TargetId& member : stage.cohort.members) {
    request.targets.push_back(member);
  }
  const Status status = health->request_probe(request);
  if (!status.ok()) {
    ++counters.evidence_rejected;
  }
}

// ---------------------------------------------------------------------------
// Stage entry
// ---------------------------------------------------------------------------

Status Orchestrator::Impl::begin_stage(Rollout& rollout, const ChangePlan& plan, StageRuntime& stage,
                                       Timestamp now, TickReport& report) {
  (void)plan;
  for (const StageId& predecessor : stage.predecessors) {
    StageRuntime* previous = rollout.find_stage(predecessor);
    if (previous == nullptr) {
      return make_status(StatusCode::kInternal,
                         "stage " + orchestrator_detail::short_hex(stage.id) +
                             " names a predecessor the rollout does not hold");
    }
    if (previous->state == StageState::kSucceeded) {
      continue;
    }
    if (is_terminal(previous->state)) {
      if (stage.policy.require_predecessors_succeeded) {
        DecisionBuilder builder(DecisionKind::kFailRollout, rollout.id, rollout.generation,
                                rollout.revision, controller_epoch, incarnation);
        builder.stage(stage.id);
        builder.input("predecessor", orchestrator_detail::short_hex(previous->id));
        builder.input("predecessor_state", to_string(previous->state));
        builder.select("refusing to enter the stage");
        builder.reject("enter the stage",
                       "a predecessor reached " + std::string(to_string(previous->state)) +
                           " without succeeding");
        builder.rationale("a later stage may not start before its predecessor's gate is satisfied");
        commit_decision(rollout, builder.finish(ids, now, limits), now);
        ++report.decisions;
        return fail_rollout(rollout,
                            "predecessor stage " + orchestrator_detail::short_hex(previous->id) +
                                " did not succeed",
                            now, report);
      }
      continue;
    }
    // The predecessor is still running: the stage simply is not ready.
    return Status::success();
  }

  Status status = Status::success();
  if (stage.state == StageState::kPending) {
    // The stage is armed first: a stage waiting at a manual entry gate is armed,
    // not pending. That distinction is what tells an operator inspecting the
    // rollout that the cohort is frozen and only the human is missing.
    status = stage_transition(rollout, stage, StageState::kArmed, now, &report);
    if (!status.ok()) {
      return status;
    }
    if (stage.policy.entry_gate == GateMode::kManual && stage.pending_gate.is_nil()) {
      DecisionBuilder builder(DecisionKind::kRefuseGate, rollout.id, rollout.generation, rollout.revision,
                              controller_epoch, incarnation);
      builder.stage(stage.id);
      builder.input("gate_mode", "manual");
      builder.select("stage entry gate opened for an operator");
      builder.reject("start the stage automatically", "the policy requires explicit approval");
      builder.rationale("the stage declares a manual entry gate, so the rollout stops here until an "
                        "operator approves it");
      commit_decision(rollout, builder.finish(ids, now, limits), now);
      ++report.decisions;
      open_gate(rollout, stage, GateKind::kStageEntry, now);
      return Status::success();
    }
  }
  status = stage_transition(rollout, stage, StageState::kRunning, now, &report);
  if (!status.ok()) {
    return status;
  }
  if (rollout.state != RolloutState::kRunning) {
    status = transition(rollout, RolloutState::kRunning,
                        "stage " + orchestrator_detail::short_hex(stage.id) + " started", now, &report);
    if (!status.ok()) {
      return status;
    }
  }
  DecisionBuilder builder(DecisionKind::kStartStage, rollout.id, rollout.generation, rollout.revision,
                          controller_epoch, incarnation);
  builder.stage(stage.id);
  builder.input_u64("members", static_cast<std::uint64_t>(stage.cohort.size()));
  builder.input_digest("membership", stage.cohort.membership_digest);
  builder.input("policy", stage.policy.describe());
  builder.select("stage started");
  builder.reject("skip to the next stage", "the deterministic plan order forbids skipping");
  builder.rationale("every predecessor is succeeded, the entry gate is open, and the cohort is "
                    "frozen at " + std::to_string(stage.cohort.size()) + " member(s)");
  commit_decision(rollout, builder.finish(ids, now, limits), now);
  ++report.decisions;
  return Status::success();
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

Status Orchestrator::Impl::create_and_dispatch(Rollout& rollout, StageRuntime& stage,
                                               const TargetRuntime& target_state,
                                               const ActionSpec& action, std::uint32_t rollback_step,
                                               AttemptEpoch epoch, Timestamp now,
                                               TickReport& report) {
  TargetRuntime* target = stage.find_target(target_state.target);
  if (target == nullptr) {
    return make_status(StatusCode::kInternal, "target vanished from its stage");
  }
  if (execution == nullptr) {
    ++counters.dispatch_refused;
    return Status::success();
  }
  const AttemptEpoch previous_epoch = target->epoch;
  const TargetState previous_state = target->state;
  const std::uint32_t previous_attempts = target->attempts_created;
  const AttemptId previous_attempt = target->current_attempt;
  const std::uint32_t previous_dispatched = stage.dispatched_total;
  const std::uint32_t previous_attempts_total = stage.attempts_total;

  Attempt attempt;
  attempt.id = ids.next<AttemptId>(IdDomain::kAttempt);
  attempt.rollout = rollout.id;
  attempt.generation = rollout.generation;
  attempt.stage = stage.id;
  attempt.cohort = stage.cohort.id;
  attempt.target = target->target;
  attempt.action = action.id;
  attempt.epoch = epoch;
  attempt.attempt_number = target->attempts_created + 1;
  attempt.incarnation = incarnation;
  attempt.action_digest = action.action_digest();
  attempt.artifact_digest = action.artifact_digest;
  attempt.idempotency_key =
      derive_idempotency_key(rollout.id, rollout.generation, stage.id, target->target, action.id);
  attempt.created_at = now;
  attempt.dispatched_at = now;
  attempt.deadline = now + stage.policy.attempt_deadline;
  attempt.state = TargetState::kDispatched;

  target->epoch = epoch;
  target->attempts_created += 1;
  // A compensation attempt leaves the target in kRollingBack: that state is what
  // tells the evidence path, and any operator reading the rollout, that this
  // attempt is undoing a change rather than making one.
  target->state = rollback_step == 0 ? TargetState::kDispatched : TargetState::kRollingBack;
  target->current_attempt = attempt.id;
  target->last_attempt = attempt.id;
  target->dispatched_at = now;
  target->last_change_at = now;
  stage.attempts_total += 1;
  stage.dispatched_total += 1;

  DispatchRequest request;
  request.fence = attempt.fence();
  request.target = target->target;
  request.cohort = stage.cohort.id;
  request.action = action.id;
  request.action_digest = attempt.action_digest;
  request.artifact_digest = attempt.artifact_digest;
  request.idempotency_key = attempt.idempotency_key;
  request.attempt_number = attempt.attempt_number;
  request.parameters = action.parameters;
  request.deadline = attempt.deadline;

  const DispatchResult result = execution->dispatch(request);
  if (!result.accepted()) {
    // The dispatch never happened. Put the target back exactly as it was so a
    // transport outage does not consume the attempt budget.
    target->epoch = previous_epoch;
    target->state = previous_state;
    target->attempts_created = previous_attempts;
    target->current_attempt = previous_attempt;
    stage.dispatched_total = previous_dispatched;
    stage.attempts_total = previous_attempts_total;
    ++counters.dispatch_refused;
    return Status::success();
  }

  note_attempt(attempt);
  note_idempotency_key(attempt.idempotency_key);
  rollout.outstanding_attempts += 1;
  ++report.dispatched;

  DecisionBuilder builder(DecisionKind::kDispatchBatch, rollout.id, rollout.generation, rollout.revision,
                          controller_epoch, incarnation);
  builder.stage(stage.id);
  builder.target(target->target);
  builder.attempt(attempt.id);
  builder.input_digest("idempotency_key", attempt.idempotency_key);
  builder.input_digest("action_digest", attempt.action_digest);
  builder.input_u64("epoch", value_of(epoch));
  builder.input("dispatch_outcome", to_string(result.outcome));
  if (rollback_step != 0) {
    builder.input_u64("rollback_step", rollback_step);
  }
  builder.select("attempt dispatched");
  builder.reject("treat the target as complete",
                 "dispatch proves acceptance only; completion requires terminal evidence");
  builder.rationale("attempt " + orchestrator_detail::short_hex(attempt.id) + " was accepted by the "
                    "execution adapter (" + std::string(to_string(result.outcome)) +
                    ") and is now fenced by generation " + rollout.generation.to_hex().substr(0, 12) +
                    " and epoch " + std::to_string(value_of(epoch)));
  commit_decision(rollout, builder.finish(ids, now, limits), now);
  ++report.decisions;
  return Status::success();
}

Status Orchestrator::Impl::dispatch_stage(Rollout& rollout, const ChangePlan& plan,
                                          StageRuntime& stage, Timestamp now, TickReport& report) {
  (void)plan;
  const DispatchPlan dispatch = plan_dispatch(rollout, stage, inventory, limits);
  for (const DispatchPlanEntry& entry : dispatch.admitted) {
    const ActionSpec* action = nullptr;
    if (entry.rollback_step == 0) {
      action = primary_action(stage);
    } else {
      action = compensation_action(stage, entry.rollback_step - 1);
    }
    if (action == nullptr) {
      continue;
    }
    const TargetRuntime* target = stage.find_target(entry.target);
    if (target == nullptr) {
      continue;
    }
    const Status status =
        create_and_dispatch(rollout, stage, *target, *action, entry.rollback_step, entry.epoch, now,
                            report);
    if (!status.ok()) {
      return status;
    }
  }
  const std::uint32_t changing = rollout.changing_count();
  stage.max_observed_changing = std::max(stage.max_observed_changing, changing);
  rollout.max_observed_changing_global = std::max(rollout.max_observed_changing_global, changing);

  if (!dispatch.rejected.empty() && dispatch.admitted.empty()) {
    DecisionBuilder builder(DecisionKind::kHoldStage, rollout.id, rollout.generation, rollout.revision,
                            controller_epoch, incarnation);
    builder.stage(stage.id);
    for (const RejectedAlternative& alternative : dispatch.rejected) {
      builder.reject(alternative.action, alternative.reason);
    }
    builder.input_u64("changing", changing);
    builder.select("no dispatch this tick");
    builder.rationale("the dispatch planner admitted no target; the reasons are recorded with the "
                      "decision");
    commit_decision(rollout, builder.finish(ids, now, limits), now);
    ++report.decisions;
  }
  return persist_rollout(rollout, now);
}

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

Status Orchestrator::Impl::cancel_in_flight(Rollout& rollout, StageRuntime& stage, Timestamp now,
                                            TickReport& report) {
  (void)report;
  for (TargetRuntime& target : stage.targets) {
    if (target.current_attempt.is_nil()) {
      continue;
    }
    auto found = attempts.find(target.current_attempt);
    if (found == attempts.end() || found->second.finished()) {
      continue;
    }
    Attempt& attempt = found->second;
    attempt.cancelled_epoch = attempt.epoch;
    if (execution != nullptr) {
      const Status status = execution->cancel(attempt.fence());
      if (!status.ok() && status.code() != StatusCode::kNotFound) {
        return status;
      }
    }
    DecisionBuilder builder(DecisionKind::kCancelAttempt, rollout.id, rollout.generation,
                            rollout.revision, controller_epoch, incarnation);
    builder.stage(stage.id);
    builder.target(target.target);
    builder.attempt(attempt.id);
    builder.input_u64("epoch", value_of(attempt.epoch));
    builder.select("attempt cancelled");
    builder.reject("let the attempt finish", "the active policy cancels in-flight work");
    builder.rationale("the attempt was cancelled; whatever it reports later is recorded but can "
                      "never be counted as a successful change");
    commit_decision(rollout, builder.finish(ids, now, limits), now);
  }
  return persist_rollout(rollout, now);
}

// ---------------------------------------------------------------------------
// Failure and rollback
// ---------------------------------------------------------------------------

Status Orchestrator::Impl::fail_rollout(Rollout& rollout, std::string reason, Timestamp now,
                                        TickReport& report) {
  if (is_terminal(rollout.state)) {
    return Status::success();
  }
  if (rollout.state == RolloutState::kPaused) {
    const Status resumed =
        transition(rollout, RolloutState::kRunning, "failure forces the rollout out of pause", now,
                   &report);
    if (!resumed.ok()) {
      return resumed;
    }
  }
  return transition(rollout, RolloutState::kFailed, std::move(reason), now,
                    &report);
}

Status Orchestrator::Impl::fail_stage(Rollout& rollout, StageRuntime& stage, std::string reason,
                                      Timestamp now, TickReport& report) {
  const Status status = stage_transition(rollout, stage, StageState::kFailed, now, &report);
  if (!status.ok()) {
    return status;
  }
  if (rollout_has_compensation(rollout)) {
    return begin_rollback(rollout, "stage failed: " + reason, now, report);
  }
  return fail_rollout(rollout, "stage " + orchestrator_detail::short_hex(stage.id) + " failed: " +
                                   reason,
                      now, report);
}

Status Orchestrator::Impl::begin_rollback(Rollout& rollout, std::string reason, Timestamp now,
                                          TickReport& report) {
  if (rollout.state != RolloutState::kRollingBack) {
    const Status status = transition(rollout, RolloutState::kRollingBack, reason, now, &report);
    if (!status.ok()) {
      return status;
    }
  }
  std::size_t marked = 0;
  for (auto stage_iterator = rollout.stages.rbegin(); stage_iterator != rollout.stages.rend();
       ++stage_iterator) {
    StageRuntime& stage = *stage_iterator;
    if (stage.state == StageState::kPending || stage.state == StageState::kSkipped) {
      continue;
    }
    if (!stage.policy.rollback_supported) {
      continue;
    }
    for (TargetRuntime& target : stage.targets) {
      if (target.state != TargetState::kSucceeded && target.state != TargetState::kFailed) {
        continue;
      }
      target.state = TargetState::kRollingBack;
      target.rollback_step = 0;
      target.current_attempt = AttemptId{};
      ++marked;
    }
  }
  DecisionBuilder builder(DecisionKind::kBeginRollback, rollout.id, rollout.generation, rollout.revision,
                          controller_epoch, incarnation);
  builder.input("reason", reason);
  builder.input_u64("targets", static_cast<std::uint64_t>(marked));
  builder.select(marked == 0 ? "no compensation is required" : "compensation started");
  builder.rationale(marked == 0
                        ? "the plan declares compensation for stages that changed no target, so "
                          "nothing is undone"
                        : std::to_string(marked) +
                              " target(s) will be compensated in reverse stage order using only the "
                              "actions the plan declared for compensation");
  commit_decision(rollout, builder.finish(ids, now, limits), now);
  ++report.decisions;
  return persist_rollout(rollout, now);
}

Status Orchestrator::Impl::run_rollback(Rollout& rollout, StageRuntime& stage, TargetRuntime& target,
                                        Timestamp now, TickReport& report) {
  if (!target.current_attempt.is_nil()) {
    const auto found = attempts.find(target.current_attempt);
    if (found != attempts.end() && !found->second.finished()) {
      return Status::success();
    }
  }
  const ActionSpec* action = compensation_action(stage, target.rollback_step);
  if (action == nullptr) {
    target.state = TargetState::kRollbackFailed;
    target.current_attempt = AttemptId{};
    target.last_reason = "the plan declares no compensation action at step " +
                         std::to_string(target.rollback_step);
    ++stage.rollback_failed_total;
    return persist_rollout(rollout, now);
  }
  const AttemptEpoch epoch = AttemptEpoch{value_of(target.epoch) + 1};
  return create_and_dispatch(rollout, stage, target, *action, target.rollback_step + 1, epoch, now,
                             report);
}

Status Orchestrator::Impl::finish_rollback(Rollout& rollout, Timestamp now, TickReport& report) {
  // Compensation is only finished when nothing is still being compensated. Failing
  // the rollout while a stage still has work outstanding would report an outcome
  // the runtime cannot yet support.
  bool any_pending = false;
  for (const StageRuntime& stage : rollout.stages) {
    if (stage.state == StageState::kPending || stage.state == StageState::kSkipped) {
      continue;
    }
    for (const TargetRuntime& target : stage.targets) {
      if (target.state == TargetState::kRollingBack) {
        any_pending = true;
        break;
      }
    }
    if (any_pending) {
      break;
    }
  }
  if (any_pending) {
    return persist_rollout(rollout, now);
  }

  for (StageRuntime& stage : rollout.stages) {
    // A stage that never ran has nothing to undo and no legal transition to
    // rolled_back; only stages that actually executed are marked compensated.
    if (stage.state == StageState::kPending || stage.state == StageState::kBlocked ||
        stage.state == StageState::kRolledBack || stage.state == StageState::kSkipped) {
    }
    const Status status = stage_transition(rollout, stage, StageState::kRolledBack, now, &report);
    if (!status.ok()) {
      return status;
    }
  }
  std::uint32_t failed_compensation = 0;
  std::uint32_t compensated = 0;
  for (const StageRuntime& stage : rollout.stages) {
    compensated += stage.rolled_back_total;
    failed_compensation += stage.rollback_failed_total;
  }
  const std::string reason = "compensation finished: " + std::to_string(compensated) +
                             " target(s) restored, " +
                             std::to_string(failed_compensation) +
                             " compensation failure(s)";
  return fail_rollout(rollout, reason, now, report);
}


// ---------------------------------------------------------------------------
// Stage progress
// ---------------------------------------------------------------------------

Status Orchestrator::Impl::complete_stage(Rollout& rollout, StageRuntime& stage, Timestamp now,
                                          TickReport& report, bool approved) {
  const HealthSourceId source = health != nullptr ? health->source_id() : HealthSourceId{};
  const HealthSnapshot snapshot = snapshot_for(stage, source, health != nullptr);
  const GateEvaluation health_evaluation =
      evaluate_health_gate(stage, stage.policy.health_gate, snapshot, now, limits);
  stage.last_health_gate = health_evaluation;
  ++report.gates_evaluated;

  const GateEvaluation evidence_evaluation =
      evaluate_evidence_gate(stage, stage.policy.evidence_gate, stage.evidence_rule, now, limits);
  stage.last_evidence_gate = evidence_evaluation;
  ++report.gates_evaluated;

  if (!evidence_evaluation.satisfied && !approved) {
    if (stage.policy.evidence_gate.mode == GateMode::kManual) {
      DecisionBuilder builder(DecisionKind::kRefuseGate, rollout.id, rollout.generation,
                              rollout.revision, controller_epoch, incarnation);
      builder.stage(stage.id);
      builder.input("reason", evidence_evaluation.reason);
      builder.select("evidence gate opened for an operator");
      builder.reject("advance automatically", "the policy requires explicit approval");
      builder.rationale("the evidence gate is manual and is not satisfied: " +
                        evidence_evaluation.reason);
      commit_decision(rollout, builder.finish(ids, now, limits), now);
      ++report.decisions;
      open_gate(rollout, stage, GateKind::kEvidence, now);
      return Status::success();
    }
    DecisionBuilder builder(DecisionKind::kFailStage, rollout.id, rollout.generation, rollout.revision,
                            controller_epoch, incarnation);
    builder.stage(stage.id);
    builder.input("reason", evidence_evaluation.reason);
    builder.select("stage failed its evidence gate");
    builder.reject("advance", evidence_evaluation.reason);
    builder.rationale("the evidence gate is automatic and cannot be satisfied: " +
                      evidence_evaluation.reason);
    commit_decision(rollout, builder.finish(ids, now, limits), now);
    ++report.decisions;
    return fail_stage(rollout, stage, evidence_evaluation.reason, now, report);
  }

  const GateEvaluation advance_evaluation = evaluate_advance_gate(stage, now, limits);
  stage.last_advance_gate = advance_evaluation;
  ++report.gates_evaluated;
  if (!advance_evaluation.satisfied && !approved) {
    if (stage.policy.advance_gate == GateMode::kManual) {
      DecisionBuilder builder(DecisionKind::kRefuseGate, rollout.id, rollout.generation,
                              rollout.revision, controller_epoch, incarnation);
      builder.stage(stage.id);
      builder.input("reason", advance_evaluation.reason);
      builder.select("advance gate opened for an operator");
      builder.reject("advance automatically", "the policy requires explicit approval");
      builder.rationale("the advance gate is manual: " + advance_evaluation.reason);
      commit_decision(rollout, builder.finish(ids, now, limits), now);
      ++report.decisions;
      open_gate(rollout, stage, GateKind::kAdvance, now);
      return Status::success();
    }
    DecisionBuilder builder(DecisionKind::kHoldStage, rollout.id, rollout.generation, rollout.revision,
                            controller_epoch, incarnation);
    builder.stage(stage.id);
    builder.input("reason", advance_evaluation.reason);
    builder.select("holding the stage");
    builder.reject("advance", advance_evaluation.reason);
    builder.rationale("the stage stays where it is and re-evaluates on the next tick: " +
                      advance_evaluation.reason);
    commit_decision(rollout, builder.finish(ids, now, limits), now);
    ++report.decisions;
    return Status::success();
  }

  Status status = stage_transition(rollout, stage, StageState::kAdvancing, now, &report);
  if (!status.ok()) {
    return status;
  }
  status = stage_transition(rollout, stage, StageState::kSucceeded, now, &report);
  if (!status.ok()) {
    return status;
  }
  rollout.cursor += 1;
  const bool finished = rollout.cursor >= rollout.stages.size();

  DecisionBuilder builder(DecisionKind::kAdvanceStage, rollout.id, rollout.generation, rollout.revision,
                          controller_epoch, incarnation);
  builder.stage(stage.id);
  builder.input_u64("healthy", health_evaluation.healthy_targets);
  builder.input_u64("failed", health_evaluation.failed_targets);
  builder.input("evidence_gate", evidence_evaluation.reason);
  builder.input("advance_gate", advance_evaluation.reason);
  builder.input_u64("cursor", rollout.cursor);
  builder.select(finished ? "rollout complete" : "advancing to the next stage");
  builder.reject("stay in this stage", "every gate this stage declares is satisfied");
  builder.rationale("stage " + orchestrator_detail::short_hex(stage.id) + " advanced: " +
                    advance_evaluation.reason +
                    (finished ? "; no stage remains, so the rollout is complete"
                              : "; the next stage in the deterministic plan order is now current"));
  commit_decision(rollout, builder.finish(ids, now, limits), now);
  ++report.decisions;

  if (finished) {
    return transition(rollout, RolloutState::kCompleted,
                      "every stage advanced and completed", now, &report);
  }
  return transition(rollout, RolloutState::kAdvancing,
                    "stage " + orchestrator_detail::short_hex(stage.id) + " advanced", now, &report);
}

Status Orchestrator::Impl::run_stage(Rollout& rollout, const ChangePlan& plan, StageRuntime& stage,
                                     Timestamp now, TickReport& report) {
  (void)plan;
  // 1. Attempt deadlines. A dispatched attempt that has not reported by its
  //    deadline is failed by the runtime, and the failure is recorded as a
  //    decision rather than silently waiting forever.
  for (TargetRuntime& target : stage.targets) {
    if (target.state != TargetState::kDispatched || target.current_attempt.is_nil()) {
      continue;
    }
    auto found = attempts.find(target.current_attempt);
    if (found == attempts.end() || found->second.finished()) {
      continue;
    }
    Attempt& attempt = found->second;
    if (attempt.deadline.is_nil() || !(now > attempt.deadline)) {
      continue;
    }
    const bool was_cancelled = value_of(attempt.cancelled_epoch) != 0;
    attempt.completed = true;
    attempt.state = was_cancelled ? TargetState::kCancelled : TargetState::kFailed;
    attempt.outcome = EvidenceOutcome::kInconclusive;
    rollout.outstanding_attempts =
        rollout.outstanding_attempts > 0 ? rollout.outstanding_attempts - 1 : 0;
    target.current_attempt = AttemptId{};
    target.completed_at = now;
    if (was_cancelled || attempt.state == TargetState::kCancelled) {
      target.state = TargetState::kCancelled;
      target.last_reason = "the attempt was cancelled and did not report before its deadline";
      ++stage.cancelled_total;
      continue;
    }
    target.state = TargetState::kFailed;
    target.outcome = EvidenceOutcome::kFailed;
    target.last_reason = "the attempt exceeded its deadline of " +
                         std::to_string(stage.policy.attempt_deadline.millis()) + " ms";
    ++stage.failed_total;
    DecisionBuilder builder(DecisionKind::kCancelAttempt, rollout.id, rollout.generation,
                            rollout.revision, controller_epoch, incarnation);
    builder.stage(stage.id);
    builder.target(target.target);
    builder.attempt(attempt.id);
    builder.input("reason", target.last_reason);
    builder.select("attempt failed by deadline");
    builder.reject("wait indefinitely",
                   "the stage policy defines a deadline for every attempt");
    builder.rationale("attempt " + orchestrator_detail::short_hex(attempt.id) +
                      " passed its deadline without terminal evidence; dispatch is not completion, "
                      "so the attempt is failed and the target may be retried under a new epoch");
    commit_decision(rollout, builder.finish(ids, now, limits), now);
    ++report.decisions;
    retry_or_exhaust(rollout, stage, target, now, report);
  }

  probe_health(rollout, stage, now);

  // 2. Failure budget.
  const std::uint64_t budget = orchestrator_detail::failure_budget(stage);
  if (stage.failed_count() > budget) {
    const std::string reason = std::to_string(stage.failed_count()) +
                               " target(s) failed, above the failure budget of " +
                               std::to_string(budget);
    DecisionBuilder builder(DecisionKind::kFailStage, rollout.id, rollout.generation, rollout.revision,
                            controller_epoch, incarnation);
    builder.stage(stage.id);
    builder.input_u64("failed", stage.failed_count());
    builder.input_u64("budget", budget);
    builder.select("stage failed");
    builder.reject("continue", reason);
    builder.rationale(reason);
    commit_decision(rollout, builder.finish(ids, now, limits), now);
    ++report.decisions;
    return fail_stage(rollout, stage, reason, now, report);
  }

  // 3. Canary clearance. The canary batch must clear before the rest of the
  //    cohort is admitted, which is a spread control in time.
  if (!stage.canary_cleared && stage.policy.canary_size > 0) {
    if (stage.dispatched_total >= stage.policy.canary_size) {
      bool canary_ok = true;
      for (const TargetRuntime& target : stage.targets) {
        if (target.attempts_created == 0) {
          continue;
        }
        if (target.state != TargetState::kSucceeded) {
          canary_ok = false;
          break;
        }
      }
      if (canary_ok && stage.policy.canary_requires_health_gate && health_gate_holds(stage, now)) {
        canary_ok = false;
      }
      if (!canary_ok) {
        // A canary that has exhausted its attempts is a failed canary, not a
        // reason to wait: the stage fails and the plan's compensation, if any,
        // takes over. Anything else falls through to dispatch so that a retry
        // inside the canary batch can still make progress.
        bool exhausted = false;
        for (const TargetRuntime& target : stage.targets) {
          if (target.attempts_created > 0 && target.state == TargetState::kExhausted) {
            exhausted = true;
            break;
          }
        }
        if (exhausted) {
          DecisionBuilder builder(DecisionKind::kFailStage, rollout.id, rollout.generation,
                                  rollout.revision, controller_epoch, incarnation);
          builder.stage(stage.id);
          builder.input_u64("canary_size", stage.policy.canary_size);
          builder.select("the canary batch failed");
          builder.reject("continue the rollout",
                         "a target in the canary batch exhausted its attempts");
          builder.rationale(
              "the canary exists to stop a bad change before it reaches the rest of the "
              "cohort; the batch did not clear, so the stage fails here rather than "
              "being held open indefinitely");
          commit_decision(rollout, builder.finish(ids, now, limits), now);
          ++report.decisions;
          return fail_stage(rollout, stage, "the canary batch failed", now, report);
        }
      }
      if (canary_ok) {
        stage.canary_cleared = true;
        DecisionBuilder builder(DecisionKind::kOpenGate, rollout.id, rollout.generation,
                                rollout.revision, controller_epoch, incarnation);
        builder.stage(stage.id);
        builder.input_u64("canary_size", stage.policy.canary_size);
        builder.select("canary cleared");
        builder.reject("admit the full cohort immediately",
                       "the canary batch must clear its health gate first");
        builder.rationale("the first " + std::to_string(stage.policy.canary_size) +
                          " target(s) completed and the health gate accepts the cohort; "
                          "the rest of the stage may now be dispatched");
        commit_decision(rollout, builder.finish(ids, now, limits), now);
        ++report.decisions;
      } else {
        // The canary has not cleared and nothing is exhausted: fall through to
        // dispatch so that a retry inside the batch can still be admitted. The
        // planner caps brand new admissions at the remaining canary slots.
      }
    }
    // When the canary batch has not been dispatched in full yet, the planner caps
    // the next batch at the remaining canary slots, so execution falls through to
    // dispatch instead of holding. Holding here would deadlock: nothing would ever
    // be dispatched, so the canary could never clear.
  }

  // 4. Everything terminal: soak, then gate.
  if (stage.terminal_count() == stage.targets.size()) {
    if (!stage.policy.evidence_gate.soak.is_zero() && stage.soak_started_at.is_nil()) {
      stage.soak_started_at = now;
      const Status status = stage_transition(rollout, stage, StageState::kSoaking, now, &report);
      if (!status.ok()) {
        return status;
      }
      if (legal_transition(rollout.state, RolloutState::kSoaking)) {
        const Status rollout_status =
            transition(rollout, RolloutState::kSoaking,
                       "soaking for " + std::to_string(stage.policy.evidence_gate.soak.millis()) +
                           " ms before the evidence gate is evaluated",
                       now, &report);
        if (!rollout_status.ok()) {
          return rollout_status;
        }
      }
      DecisionBuilder builder(DecisionKind::kHoldStage, rollout.id, rollout.generation,
                              rollout.revision, controller_epoch, incarnation);
      builder.stage(stage.id);
      builder.input_u64("soak_ms", stage.policy.evidence_gate.soak.millis());
      builder.select("soaking");
      builder.reject("evaluate the evidence gate now",
                     "the soak duration has not elapsed");
      builder.rationale("the stage is soaking; evidence gathered before the soak completes cannot "
                        "show whether the change is stable");
      commit_decision(rollout, builder.finish(ids, now, limits), now);
      ++report.decisions;
      return Status::success();
    }
    // The evidence gate cannot be evaluated until the soak has elapsed. Calling
    // it early would fail the stage for a gate that simply is not due yet.
    if (!stage.policy.evidence_gate.soak.is_zero()) {
      const Duration elapsed = now - stage.soak_started_at;
      if (elapsed < stage.policy.evidence_gate.soak) {
        return persist_rollout(rollout, now);
      }
    }
    return complete_stage(rollout, stage, now, report, false);
  }

  // 5. Health hold: an unhealthy population stops new work without failing the
  //    stage on its own.
  if (stage.succeeded_total > 0 && health_gate_holds(stage, now)) {
    const HealthSourceId source = health != nullptr ? health->source_id() : HealthSourceId{};
    const HealthSnapshot snapshot = snapshot_for(stage, source, health != nullptr);
    const GateEvaluation evaluation =
        evaluate_health_gate(stage, stage.policy.health_gate, snapshot, now, limits);
    stage.last_health_gate = evaluation;
    DecisionBuilder builder(DecisionKind::kHoldStage, rollout.id, rollout.generation, rollout.revision,
                            controller_epoch, incarnation);
    builder.stage(stage.id);
    builder.input("reason", evaluation.reason);
    for (const std::string& blocking : evaluation.blocking) {
      builder.reject("dispatch", blocking);
    }
    builder.input_u64("healthy", evaluation.healthy_targets);
    builder.input_u64("total", evaluation.total_targets);
    builder.select("holding dispatch");
    builder.rationale("the cohort is not healthy enough to admit more change: " + evaluation.reason);
    commit_decision(rollout, builder.finish(ids, now, limits), now);
    ++report.decisions;
    return persist_rollout(rollout, now);
  }

  return dispatch_stage(rollout, plan, stage, now, report);
}

// ---------------------------------------------------------------------------
// Rollout advancement
// ---------------------------------------------------------------------------

void Orchestrator::Impl::advance(Rollout& rollout, const ChangePlan& plan, Timestamp now,
                                 TickReport& report) {
  switch (rollout.state) {
    case RolloutState::kCreated:
    case RolloutState::kValidated:
    case RolloutState::kArmed:
    case RolloutState::kRetired:
      return;
    case RolloutState::kPaused:
      // In-flight attempts may still report, and their terminal states are
      // recorded, but no gate is evaluated and no work is admitted.
      return;
    case RolloutState::kGated: {
      StageRuntime* stage = rollout.current_stage();
      if (stage != nullptr && stage->pending_gate.is_nil() && stage->state == StageState::kGated) {
        // A gate that is no longer pending can only be resolved by a command.
        return;
      }
      if (stage != nullptr) {
        probe_health(rollout, *stage, now);
      }
      return;
    }
    case RolloutState::kAborting:
      return;
    case RolloutState::kRollingBack: {
      for (StageRuntime& stage : rollout.stages) {
        for (TargetRuntime& target : stage.targets) {
          if (target.state != TargetState::kRollingBack) {
            continue;
          }
          const Status status = run_rollback(rollout, stage, target, now, report);
          if (!status.ok()) {
            storage_failed = true;
            storage_status = status;
            return;
          }
        }
      }
      const Status status = finish_rollback(rollout, now, report);
      if (!status.ok()) {
        storage_failed = true;
        storage_status = status;
      }
      return;
    }
    case RolloutState::kFailed:
    case RolloutState::kCompleted:
      return;
    case RolloutState::kRunning:
    case RolloutState::kSoaking:
    case RolloutState::kAdvancing:
      break;
  }

  StageRuntime* stage = rollout.current_stage();
  if (stage == nullptr) {
    const Status status = transition(rollout, RolloutState::kCompleted,
                                     "every stage in the plan has been processed", now, &report);
    if (!status.ok()) {
      storage_failed = true;
      storage_status = status;
    }
    return;
  }
  Status status = Status::success();
  switch (stage->state) {
    case StageState::kPending:
      status = begin_stage(rollout, plan, *stage, now, report);
      break;
    case StageState::kArmed:
      status = stage_transition(rollout, *stage, StageState::kRunning, now, &report);
      break;
    case StageState::kRunning:
    case StageState::kSoaking:
      status = run_stage(rollout, plan, *stage, now, report);
      break;
    case StageState::kGated:
      probe_health(rollout, *stage, now);
      break;
    case StageState::kAdvancing:
      status = complete_stage(rollout, *stage, now, report, true);
      break;
    case StageState::kSucceeded:
      // The stage is done but the cursor has not moved: this happens only if a
      // previous attempt to advance was interrupted. Re-advance deterministically.
      rollout.cursor += 1;
      if (rollout.cursor >= rollout.stages.size()) {
        status = transition(rollout, RolloutState::kCompleted, "every stage completed", now, &report);
      } else {
        status = transition(rollout, RolloutState::kAdvancing, "advancing", now, &report);
      }
      break;
    case StageState::kFailed:
    case StageState::kAborted:
      if (rollout_has_compensation(rollout)) {
        status = begin_rollback(rollout, "a stage failed", now, report);
      } else {
        status = fail_rollout(rollout, "a stage failed", now, report);
      }
      break;
    case StageState::kRolledBack:
    case StageState::kSkipped:
      rollout.cursor += 1;
      break;
  }
  if (!status.ok()) {
    storage_failed = true;
    storage_status = status;
  }
}

}  // namespace rollout_fabric
