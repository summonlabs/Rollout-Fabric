// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic gate evaluation. These functions are pure: they read a stage's
// recorded state and return an evaluation. They hold no lock, allocate nothing
// unbounded, and produce byte-identical output for identical input, which is
// what makes the determinism tests meaningful and the explanations trustworthy.
#include <algorithm>
#include <string>

#include "rollout_fabric/orchestrator.hpp"

namespace rollout_fabric {
namespace {

[[nodiscard]] std::string describe_ratio(const Ratio& value) {
  return std::to_string(value.numerator) + "/" + std::to_string(value.denominator);
}

// Health is usable by a gate only when the observation behind it was live and
// named an attesting source. A restored boolean is not an observation.
[[nodiscard]] bool health_is_live(const TargetRuntime& target) noexcept {
  return target.last_health_origin_live;
}

}  // namespace

GateEvaluation evaluate_health_gate(const StageRuntime& stage, const HealthGatePolicy& policy,
                                    const HealthSnapshot& snapshot, Timestamp now,
                                    const RuntimeLimits& limits) {
  GateEvaluation evaluation;
  evaluation.kind = GateKind::kHealth;
  evaluation.stage = stage.id;
  evaluation.evaluated_at = now;
  evaluation.total_targets = static_cast<std::uint32_t>(stage.targets.size());
  evaluation.healthy_targets = stage.healthy_count();
  evaluation.failed_targets = stage.failed_count();
  evaluation.pending_targets = stage.pending_count();

  ByteWriter digest_input;
  digest_input.string("rollout-fabric/health-gate/v1");
  digest_input.u32(evaluation.total_targets);
  digest_input.u32(evaluation.healthy_targets);
  digest_input.u32(evaluation.failed_targets);
  digest_input.u32(evaluation.pending_targets);
  digest_input.i64(snapshot.observed_at.unix_nanos());
  digest_input.boolean(snapshot.live);
  digest_input.raw(std::span<const std::byte>(snapshot.digest.data(), snapshot.digest.size()));

  if (!policy.enabled) {
    evaluation.satisfied = true;
    evaluation.reason = "the health gate is disabled for this stage";
    evaluation.evidence_digest = sha256(digest_input.span());
    return evaluation;
  }

  const Duration max_age = policy.max_age.is_zero()
                               ? Duration::from_millis(limits.default_evidence_max_age_millis)
                               : policy.max_age;
  const Duration skew = Duration::from_millis(limits.max_future_skew_millis);

  bool satisfied = true;
  if (!snapshot.live) {
    satisfied = false;
    evaluation.reason = "no live health observation is available for this cohort";
    evaluation.blocking.push_back("the most recent health view was not attested live");
  } else {
    const Freshness freshness = evaluate_freshness(snapshot.observed_at, now, max_age, skew);
    digest_input.string(freshness.reason);
    if (!freshness.fresh) {
      satisfied = false;
      evaluation.reason = "the health observation cannot be used: " + freshness.reason;
      evaluation.blocking.push_back(freshness.reason);
    } else if (!policy.required_source.is_nil() && snapshot.source != policy.required_source) {
      satisfied = false;
      evaluation.reason = "the health observation came from a source the policy does not accept";
      evaluation.blocking.push_back("attesting source " + snapshot.source.to_hex().substr(0, 12) +
                                    " is not the required source " +
                                    policy.required_source.to_hex().substr(0, 12));
    } else if (!policy.min_healthy_fraction.satisfied_by(evaluation.healthy_targets,
                                                         evaluation.total_targets)) {
      satisfied = false;
      evaluation.blocking.push_back("healthy fraction " +
                                    std::to_string(evaluation.healthy_targets) + "/" +
                                    std::to_string(evaluation.total_targets) + " is below " +
                                    describe_ratio(policy.min_healthy_fraction));
    }
  }

  if (satisfied) {
    // Every target the gate counts must have been observed healthy often
    // enough, consecutively, at a live observation time. A restored boolean is
    // not enough: the observation that produced it has to be live.
    for (const TargetRuntime& target : stage.targets) {
      if (target.state != TargetState::kSucceeded) {
        continue;
      }
      if (!target.healthy || target.consecutive_healthy < policy.min_consecutive_healthy ||
          !health_is_live(target)) {
        satisfied = false;
        evaluation.blocking.push_back("target " + target.target.to_hex().substr(0, 12) + " has " +
                                      std::to_string(target.consecutive_healthy) +
                                      " consecutive live healthy observation(s)");
        if (evaluation.blocking.size() >= 16) {
          break;
        }
      }
    }
  }

  const std::uint64_t budget_by_count = policy.failure_budget_targets;
  const std::uint64_t budget_by_fraction =
      policy.failure_budget_fraction.apply_floor(static_cast<std::uint64_t>(stage.targets.size()));
  const std::uint64_t budget = std::max(budget_by_count, budget_by_fraction);
  if (evaluation.failed_targets > budget) {
    satisfied = false;
    evaluation.blocking.push_back("failed targets " + std::to_string(evaluation.failed_targets) +
                                  " exceed the health failure budget " + std::to_string(budget));
  }

  evaluation.satisfied = satisfied;
  if (satisfied) {
    evaluation.reason = "healthy " + std::to_string(evaluation.healthy_targets) + "/" +
                        std::to_string(evaluation.total_targets) + " under a live observation";
  } else if (evaluation.reason.empty()) {
    evaluation.reason = "the cohort has not been observed healthy";
  }
  evaluation.evidence_digest = sha256(digest_input.span());
  return evaluation;
}

GateEvaluation evaluate_evidence_gate(const StageRuntime& stage, const EvidenceGatePolicy& policy,
                                      const TargetEvidenceRule& rule, Timestamp now,
                                      const RuntimeLimits& limits) {
  GateEvaluation evaluation;
  evaluation.kind = GateKind::kEvidence;
  evaluation.stage = stage.id;
  evaluation.evaluated_at = now;
  evaluation.total_targets = static_cast<std::uint32_t>(stage.targets.size());
  evaluation.healthy_targets = stage.healthy_count();
  evaluation.failed_targets = stage.failed_count();
  evaluation.pending_targets = stage.pending_count();
  evaluation.verified_targets = stage.verified_count();

  ByteWriter digest_input;
  digest_input.string("rollout-fabric/evidence-gate/v1");
  digest_input.u32(evaluation.total_targets);
  digest_input.u32(evaluation.healthy_targets);
  digest_input.u32(evaluation.failed_targets);
  digest_input.u32(evaluation.verified_targets);

  const Duration max_age = policy.max_age.is_zero()
                               ? Duration::from_millis(limits.default_evidence_max_age_millis)
                               : policy.max_age;

  // Soak: the gate cannot be evaluated before the change has been observed for
  // the configured duration.
  if (!policy.soak.is_zero()) {
    if (stage.soak_started_at.is_nil()) {
      evaluation.reason = "soak has not started for this stage";
      evaluation.evidence_digest = sha256(digest_input.span());
      return evaluation;
    }
    const Duration elapsed = now - stage.soak_started_at;
    digest_input.i64(elapsed.nanos());
    if (elapsed < policy.soak) {
      evaluation.reason = "soaking: " + std::to_string(elapsed.millis()) + " ms of " +
                          std::to_string(policy.soak.millis()) + " ms elapsed";
      evaluation.evidence_digest = sha256(digest_input.span());
      return evaluation;
    }
  }

  const std::uint32_t terminal = stage.terminal_count();
  if (terminal < stage.targets.size()) {
    evaluation.reason = "waiting for " +
                        std::to_string(stage.targets.size() - terminal) +
                        " target(s) to reach a terminal state";
    evaluation.evidence_digest = sha256(digest_input.span());
    return evaluation;
  }

  if (stage.failed_count() > 0) {
    evaluation.reason = std::to_string(stage.failed_count()) +
                        " target(s) failed, so the stage cannot advance";
    evaluation.evidence_digest = sha256(digest_input.span());
    return evaluation;
  }

  if (!rule.require_verification) {
    evaluation.satisfied = true;
    evaluation.reason = "every target reached a terminal success and no verification is required";
    evaluation.evidence_digest = sha256(digest_input.span());
    return evaluation;
  }

  std::uint32_t verified = 0;
  for (const TargetRuntime& target : stage.targets) {
    if (target.state != TargetState::kSucceeded) {
      continue;
    }
    if (target.verification_observed_at.is_nil()) {
      evaluation.blocking.push_back("target " + target.target.to_hex().substr(0, 12) +
                                    " has no verification result");
      continue;
    }
    const Freshness freshness =
        evaluate_freshness(target.verification_observed_at, now, max_age,
                           Duration::from_millis(limits.max_future_skew_millis));
    if (!freshness.fresh) {
      evaluation.blocking.push_back("target " + target.target.to_hex().substr(0, 12) +
                                    " verification is unusable: " + freshness.reason);
      continue;
    }
    if (!target.verification_passed) {
      evaluation.blocking.push_back("target " + target.target.to_hex().substr(0, 12) +
                                    " failed verification");
      continue;
    }
    if (!policy.required_verifier.is_nil() && target.verification_verifier != policy.required_verifier) {
      evaluation.blocking.push_back("target " + target.target.to_hex().substr(0, 12) +
                                    " was verified by a different verifier");
      continue;
    }
    ++verified;
  }
  evaluation.verified_targets = verified;
  digest_input.u32(verified);

  if (evaluation.blocking.size() > 16) {
    evaluation.blocking.resize(16);
  }

  const std::uint64_t required_by_count = policy.min_passing_samples;
  const std::uint64_t required_by_fraction =
      policy.min_passing_fraction.apply_ceil(static_cast<std::uint64_t>(stage.targets.size()));
  const std::uint64_t required = std::max(required_by_count, required_by_fraction);
  if (policy.require_full_coverage) {
    evaluation.satisfied = verified == stage.targets.size();
  } else {
    evaluation.satisfied = verified >= required;
  }
  if (evaluation.satisfied) {
    evaluation.reason = "verification passed for " + std::to_string(verified) + "/" +
                        std::to_string(stage.targets.size()) + " target(s)";
  } else {
    evaluation.reason = "verification passed for only " + std::to_string(verified) + "/" +
                        std::to_string(stage.targets.size()) + " target(s); " +
                        std::to_string(required) + " required";
  }
  evaluation.evidence_digest = sha256(digest_input.span());
  return evaluation;
}

GateEvaluation evaluate_advance_gate(const StageRuntime& stage, Timestamp now,
                                     const RuntimeLimits& limits) {
  (void)limits;
  GateEvaluation evaluation;
  evaluation.kind = GateKind::kAdvance;
  evaluation.stage = stage.id;
  evaluation.evaluated_at = now;
  evaluation.total_targets = static_cast<std::uint32_t>(stage.targets.size());
  evaluation.healthy_targets = stage.healthy_count();
  evaluation.failed_targets = stage.failed_count();
  evaluation.pending_targets = stage.pending_count();

  ByteWriter digest_input;
  digest_input.string("rollout-fabric/advance-gate/v1");
  digest_input.u32(evaluation.total_targets);
  digest_input.u32(evaluation.failed_targets);
  digest_input.u32(evaluation.pending_targets);
  digest_input.u32(evaluation.healthy_targets);

  const std::uint64_t budget_by_count = stage.policy.failure_budget_targets;
  const std::uint64_t budget_by_fraction = stage.policy.failure_budget_fraction.apply_floor(
      static_cast<std::uint64_t>(stage.targets.size()));
  const std::uint64_t budget = std::max(budget_by_count, budget_by_fraction);
  digest_input.u64(budget);

  if (evaluation.pending_targets != 0) {
    evaluation.reason = std::to_string(evaluation.pending_targets) + " target(s) are still pending";
    evaluation.evidence_digest = sha256(digest_input.span());
    return evaluation;
  }
  if (evaluation.failed_targets > budget) {
    evaluation.reason = "failed targets " + std::to_string(evaluation.failed_targets) +
                        " exceed the failure budget " + std::to_string(budget);
    evaluation.evidence_digest = sha256(digest_input.span());
    return evaluation;
  }
  const bool healthy_enough = stage.policy.min_healthy_fraction.satisfied_by(
      evaluation.healthy_targets, static_cast<std::uint64_t>(stage.targets.size()));
  digest_input.boolean(healthy_enough);
  if (!healthy_enough) {
    evaluation.reason = "healthy fraction " + std::to_string(evaluation.healthy_targets) + "/" +
                        std::to_string(stage.targets.size()) + " is below " +
                        describe_ratio(stage.policy.min_healthy_fraction);
    evaluation.evidence_digest = sha256(digest_input.span());
    return evaluation;
  }
  evaluation.satisfied = true;
  evaluation.reason = "every target is terminal, " + std::to_string(evaluation.failed_targets) +
                      " failure(s) are within the budget of " + std::to_string(budget) +
                      ", and the healthy fraction is met";
  evaluation.evidence_digest = sha256(digest_input.span());
  return evaluation;
}

}  // namespace rollout_fabric
