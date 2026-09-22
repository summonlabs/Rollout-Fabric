// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// THE SIMULATOR. Nothing here touches a device. It exists so that the
// orchestration semantics can be driven deterministically and adversarially.
// The distributed claims of this repository are proved by the process-backed
// adapter, not by this file.
#include "rollout_fabric/sim_adapters.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace rollout_fabric::sim {
namespace {

// A file-local factory keeps identifier minting unique across simulator
// instances and deterministic in order of use within a run.
[[nodiscard]] IdFactory& sim_ids() {
  static IdFactory factory = IdFactory::from_entropy();
  return factory;
}

}  // namespace

SimExecutionAdapter::SimExecutionAdapter(const Clock& clock, RuntimeLimits limits)
    : clock_(&clock), limits_(limits) {}

void SimExecutionAdapter::set_behavior(const TargetId& target, TargetBehavior behavior) {
  behaviors_[target] = behavior;
}

TargetBehavior SimExecutionAdapter::behavior_for(const TargetId& target) const {
  const auto found = behaviors_.find(target);
  if (found == behaviors_.end()) {
    return default_behavior_;
  }
  return found->second;
}

bool SimExecutionAdapter::has_effect(const Digest256& idempotency_key) const noexcept {
  return effects_.find(idempotency_key) != effects_.end();
}

std::size_t SimExecutionAdapter::in_flight() const noexcept {
  std::size_t count = 0;
  for (const auto& entry : effects_) {
    if (!entry.second.terminal) {
      ++count;
    }
  }
  return count;
}

DispatchResult SimExecutionAdapter::dispatch(const DispatchRequest& request) {
  DispatchResult result;
  ++stats_.dispatched;
  const Timestamp now = clock_->wall_now();
  if (!available_) {
    ++stats_.dispatch_refused;
    result.outcome = DispatchOutcome::kUnavailable;
    result.detail = "the simulated execution plane is not available";
    return result;
  }
  const TargetBehavior behavior = behavior_for(request.target);
  if (behavior.refuse_dispatch) {
    ++stats_.dispatch_refused;
    result.outcome = DispatchOutcome::kRejected;
    result.detail = "the simulated executor refused the dispatch";
    return result;
  }

  const auto existing = effects_.find(request.idempotency_key);
  if (existing != effects_.end()) {
    // Exactly-once effects: the key is already known, so the recorded outcome
    // is reported again instead of the effect being performed a second time.
    existing->second.reported_terminal = false;
    existing->second.extra_report_pending = existing->second.terminal;
    ++stats_.dispatch_accepted;
    result.outcome = DispatchOutcome::kDuplicateSuppressed;
    result.detail = "the simulated executor already performed this effect";
    result.accepted_at = existing->second.accepted_at;
    return result;
  }

  if (effects_.size() >= limits_.max_idempotency_entries) {
    // Bound the effect table. Only a finished effect may be evicted, and the
    // choice is deterministic: the lowest key in the ordered map.
    bool evicted = false;
    for (auto iterator = effects_.begin(); iterator != effects_.end(); ++iterator) {
      if (!iterator->second.terminal) {
        continue;
      }
      const Digest256 victim = iterator->first;
      live_.erase(std::remove_if(live_.begin(), live_.end(),
                                 [&](const Digest256& key) { return digest_equal(key, victim); }),
                  live_.end());
      effects_.erase(iterator);
      evicted = true;
      break;
    }
    if (!evicted) {
      ++stats_.dispatch_refused;
      result.outcome = DispatchOutcome::kThrottled;
      result.detail = "the simulated executor has no room for another effect";
      return result;
    }
  }

  Effect effect;
  effect.fence = request.fence;
  effect.idempotency_key = request.idempotency_key;
  effect.target = request.target;
  effect.cohort = request.cohort;
  effect.action = request.action;
  effect.action_digest = request.action_digest;
  effect.accepted_at = now;
  effect.due_at = now + behavior.work_duration + behavior.report_delay;
  effect.behavior = behavior;
  effect.epoch = request.fence.epoch;
  effect.sequence = 0;
  effects_.emplace(request.idempotency_key, std::move(effect));
  live_.push_back(request.idempotency_key);
  ++stats_.dispatch_accepted;
  result.outcome = DispatchOutcome::kAccepted;
  result.detail = "the simulated executor accepted the attempt";
  result.accepted_at = now;
  return result;
}

void SimExecutionAdapter::poll(IEvidenceSink& sink) {
  const Timestamp now = clock_->wall_now();
  // Iterate over the insertion-ordered key list so that evidence is delivered
  // in a deterministic order regardless of key ordering.
  const std::vector<Digest256> keys = live_;
  for (const Digest256& key : keys) {
    const auto found = effects_.find(key);
    if (found == effects_.end()) {
      continue;
    }
    Effect& effect = found->second;

    const auto emit = [&](EvidenceKind kind, EvidenceOutcome outcome, std::uint64_t sequence,
                          Timestamp observed_at) {
      EvidenceRecord record;
      record.id = sim_ids().next<EvidenceId>(IdDomain::kEvidence);
      record.rollout = effect.fence.rollout;
      record.generation = effect.fence.generation;
      record.stage = effect.fence.stage;
      record.cohort = effect.cohort;
      record.target = effect.target;
      record.attempt = effect.fence.attempt;
      record.attempt_epoch = effect.fence.epoch;
      record.incarnation = effect.fence.incarnation;
      record.kind = kind;
      record.outcome = outcome;
      record.authority = EvidenceAuthority::kExecutionAdapterAttested;
      record.origin = EvidenceOrigin::kLive;
      record.sequence = Sequence{sequence};
      record.observed_at = observed_at;
      record.recorded_at = clock_->wall_now();
      record.payload_digest = sha256(std::string(to_string(kind)));
      ++stats_.evidence_delivered;
      sink.on_evidence(std::move(record));
    };

    if (effect.sequence == 0) {
      // Acceptance proves only that the executor took the work.
      effect.sequence = 1;
      emit(EvidenceKind::kDispatchAccepted, EvidenceOutcome::kSucceeded, 1, now);
      continue;
    }

    if (effect.extra_report_pending) {
      effect.extra_report_pending = false;
      if (effect.behavior.reorder_report) {
        // A stale record that arrives after a newer one has already been
        // applied. It must not change anything.
        emit(EvidenceKind::kExecutionProgress, EvidenceOutcome::kInconclusive, 1,
             effect.accepted_at);
      } else {
        // The identical terminal record, delivered twice.
        emit(effect.completed ? EvidenceKind::kExecutionCompleted : EvidenceKind::kExecutionFailed,
             effect.completed ? EvidenceOutcome::kSucceeded : EvidenceOutcome::kFailed,
             static_cast<std::uint64_t>(effect.sequence), effect.due_at);
      }
      continue;
    }

    if (effect.terminal || now < effect.due_at) {
      continue;
    }

    if (effect.behavior.never_complete) {
      // The worker accepted the attempt and will never report a terminal
      // outcome. Dispatch is not completion; this is that case in its purest
      // form and the orchestrator must not treat the target as done.
      continue;
    }

    effect.completed = !effect.behavior.fail;
    if (drop_evidence_ > 0) {
      --drop_evidence_;
      ++stats_.evidence_rejected;
      effect.terminal = true;
      effect.reported_terminal = true;
      continue;
    }
    effect.terminal = true;
    effect.reported_terminal = true;
    effect.sequence += 1;
    emit(effect.completed ? EvidenceKind::kExecutionCompleted : EvidenceKind::kExecutionFailed,
         effect.completed ? EvidenceOutcome::kSucceeded : EvidenceOutcome::kFailed,
         static_cast<std::uint64_t>(effect.sequence), effect.due_at);
    if (effect.behavior.duplicate_report || effect.behavior.reorder_report) {
      effect.extra_report_pending = true;
    }
  }
}

ReconcileResult SimExecutionAdapter::reconcile(const std::vector<AttemptFence>& attempts) {
  ++stats_.reconcile_requests;
  ReconcileResult result;
  result.ok = true;
  for (const AttemptFence& fence : attempts) {
    bool matched = false;
    for (const auto& entry : effects_) {
      if (!(entry.second.fence.attempt == fence.attempt)) {
        continue;
      }
      matched = true;
      ReconcileEntry answer;
      answer.fence = fence;
      if (!entry.second.terminal) {
        answer.disposition = ReconcileDisposition::kInProgress;
        answer.outcome = EvidenceOutcome::kInconclusive;
        answer.detail = "the effect is still running";
      } else {
        answer.disposition =
            entry.second.completed ? ReconcileDisposition::kCompleted : ReconcileDisposition::kFailed;
        answer.outcome = entry.second.completed ? EvidenceOutcome::kSucceeded
                                                : EvidenceOutcome::kFailed;
        answer.detail = "recovered from the executor's recorded outcome";
      }
      result.entries.push_back(std::move(answer));
      break;
    }
    if (!matched) {
      ReconcileEntry answer;
      answer.fence = fence;
      answer.disposition = ReconcileDisposition::kNotStarted;
      answer.outcome = EvidenceOutcome::kInconclusive;
      answer.detail = "the executor has no record of this idempotency key";
      result.entries.push_back(std::move(answer));
    }
  }
  return result;
}

Status SimExecutionAdapter::cancel(const AttemptFence& fence) {
  for (auto& entry : effects_) {
    if (!(entry.second.fence.attempt == fence.attempt)) {
      continue;
    }
    if (entry.second.terminal) {
      return make_status(StatusCode::kNotFound, "the effect already reached an outcome");
    }
    entry.second.terminal = true;
    entry.second.completed = false;
    entry.second.reported_terminal = true;
    entry.second.extra_report_pending = false;
    // The acknowledgement is delivered as evidence, exactly as the
    // process-backed worker delivers it, so the orchestrator reaches the
    // cancelled state through the same path in both deployments.
    if (sink_ != nullptr) {
      EvidenceRecord record;
      record.id = sim_ids().next<EvidenceId>(IdDomain::kEvidence);
      record.rollout = entry.second.fence.rollout;
      record.generation = entry.second.fence.generation;
      record.stage = entry.second.fence.stage;
      record.cohort = entry.second.cohort;
      record.target = entry.second.target;
      record.attempt = entry.second.fence.attempt;
      record.attempt_epoch = entry.second.fence.epoch;
      record.incarnation = entry.second.fence.incarnation;
      record.kind = EvidenceKind::kCancellationAcknowledged;
      record.outcome = EvidenceOutcome::kInconclusive;
      record.authority = EvidenceAuthority::kExecutionAdapterAttested;
      record.origin = EvidenceOrigin::kLive;
      record.sequence = Sequence{static_cast<std::uint64_t>(entry.second.sequence) + 1};
      record.observed_at = clock_->wall_now();
      record.recorded_at = record.observed_at;
      record.payload_digest = sha256(std::string("cancelled"));
      sink_->on_evidence(std::move(record));
    }
    return Status::success();
  }
  return make_status(StatusCode::kNotFound, "no such attempt is known to the simulated executor");
}

void SimExecutionAdapter::make_all_due() {
  const Timestamp now = clock_->wall_now();
  for (auto& entry : effects_) {
    entry.second.due_at = now;
  }
}

void SimExecutionAdapter::forget_all() {
  effects_.clear();
  live_.clear();
}

SimHealthAdapter::SimHealthAdapter(const Clock& clock, RuntimeLimits limits, HealthSourceId source)
    : clock_(&clock), limits_(limits), source_(source) {}

void SimHealthAdapter::set_healthy(const TargetId& target, bool healthy) { health_[target] = healthy; }


Status SimHealthAdapter::request_probe(const HealthProbeRequest& request) {
  ++stats_.dispatched;
  if (!available_) {
    return make_status(StatusCode::kUnavailable, "the simulated health source is not available");
  }
  if (!failure_.ok()) {
    return failure_;
  }
  if (request.targets.size() > limits_.max_total_targets) {
    return make_status(StatusCode::kLimitExceeded,
                       "the probe names more targets than max_total_targets");
  }
  const Timestamp observed_at = clock_->wall_now() - health_age_;
  last_observed_at_ = observed_at;
  if (sink_ == nullptr) {
    return Status::success();
  }
  for (const TargetId& target : request.targets) {
    const auto found = health_.find(target);
    const bool healthy = found == health_.end() ? true : found->second;
    EvidenceRecord record;
    record.id = sim_ids().next<EvidenceId>(IdDomain::kEvidence);
    record.target = target;
    record.kind = EvidenceKind::kHealthSample;
    record.outcome = healthy ? EvidenceOutcome::kSucceeded : EvidenceOutcome::kFailed;
    record.authority = EvidenceAuthority::kHealthSourceAttested;
    record.origin = EvidenceOrigin::kLive;
    record.sequence = Sequence{0};
    record.observed_at = observed_at;
    record.recorded_at = clock_->wall_now();
    record.health_source = source_;
    record.payload_digest = sha256(std::string(healthy ? "healthy" : "unhealthy"));
    ++stats_.evidence_delivered;
    sink_->on_evidence(std::move(record));
  }
  return Status::success();
}

HealthSnapshotView SimHealthAdapter::latest(const std::vector<TargetId>& targets) const {
  HealthSnapshotView view;
  view.source = source_;
  view.observed_at = last_observed_at_;
  view.live = live_;
  view.available = available_;
  for (const TargetId& target : targets) {
    HealthSample sample;
    sample.target = target;
    const auto found = health_.find(target);
    sample.healthy = found == health_.end() ? true : found->second;
    sample.observed_at = last_observed_at_;
    sample.reason = sample.healthy ? "reported healthy" : "reported unhealthy";
    view.samples.push_back(std::move(sample));
  }
  return view;
}

}  // namespace rollout_fabric::sim
