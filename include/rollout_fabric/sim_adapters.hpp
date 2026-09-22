// Rollout Fabric - in-process simulated execution and health planes.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// THIS IS A SIMULATOR. It performs no change on any device. It exists so that
// the orchestration semantics - gating, fencing, blast radius, restart
// recovery - can be driven deterministically and adversarially at a speed a
// real fleet cannot offer, and so that failure modes that are hard to provoke
// on hardware (a worker that never reports, a completion report that arrives
// twice, health telemetry that is minutes old) can be injected exactly.
//
// Distributed claims are NOT proved here. They are proved by the process-backed
// adapter, which speaks the real protocol over real sockets to real worker
// processes.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "rollout_fabric/adapters.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/time.hpp"

namespace rollout_fabric::sim {

struct TargetBehavior {
  // How long the simulated effect takes, measured on the supplied clock.
  Duration work_duration = Duration::from_millis(10);
  // The effect fails.
  bool fail = false;
  // Compensation for this target fails.
  bool rollback_fails = false;
  // The worker accepts the attempt and never reports a terminal outcome. This
  // is the "dispatch is not completion" case in its purest form.
  bool never_complete = false;
  // Terminal evidence is emitted twice with the same sequence.
  bool duplicate_report = false;
  // Terminal evidence is emitted with a lower sequence after a higher one.
  bool reorder_report = false;
  // Accepted but refused by the adapter (capacity, fencing, transport down).
  bool refuse_dispatch = false;
  // Health samples for this target are reported with a stale observation time.
  bool stale_health = false;
  // Health samples report unhealthy unconditionally.
  bool always_unhealthy = false;
  // Extra delay before the terminal evidence becomes visible.
  Duration report_delay = Duration::from_millis(0);
};

class SimExecutionAdapter final : public IExecutionAdapter {
 public:
  SimExecutionAdapter(const Clock& clock, RuntimeLimits limits);

  [[nodiscard]] std::string_view name() const noexcept override { return "sim-execution"; }
  [[nodiscard]] bool available() const noexcept override { return available_; }
  [[nodiscard]] DispatchResult dispatch(const DispatchRequest& request) override;
  [[nodiscard]] ReconcileResult reconcile(const std::vector<AttemptFence>& attempts) override;
  [[nodiscard]] Status cancel(const AttemptFence& fence) override;
  void poll(IEvidenceSink& sink) override;
  [[nodiscard]] AdapterStats stats() const override { return stats_; }

  // The sink the orchestrator registers. Cancellation acknowledgements are
  // delivered through it, exactly as the process-backed worker delivers them.
  void set_sink(IEvidenceSink* sink) noexcept { sink_ = sink; }

  // --- test and example controls ---
  void set_behavior(const TargetId& target, TargetBehavior behavior);
  void set_default_behavior(TargetBehavior behavior) { default_behavior_ = behavior; }
  void set_available(bool available) noexcept { available_ = available; }
  // Drops the next n pieces of evidence the adapter would have delivered.
  void drop_next_evidence(std::uint32_t count) noexcept { drop_evidence_ = count; }
  // Forgets everything: used to simulate an executor that lost its memory, so
  // that only the durable state of the controller can carry the rollout.
  void forget_all();
  // Effects that have been accepted and have not yet reached their outcome.
  [[nodiscard]] std::size_t in_flight() const noexcept;
  [[nodiscard]] std::size_t known_effects() const noexcept { return effects_.size(); }
  [[nodiscard]] bool has_effect(const Digest256& idempotency_key) const noexcept;
  // Forces every outstanding attempt to become deliverable now.
  void make_all_due();

 private:
  struct Effect {
    AttemptFence fence{};
    Digest256 idempotency_key{};
    TargetId target{};
    CohortId cohort{};
    ActionId action{};
    Digest256 action_digest{};
    Timestamp accepted_at{};
    Timestamp due_at{};
    std::int64_t sequence = 0;
    bool terminal = false;
    bool completed = false;
    bool reported_terminal = false;
    bool extra_report_pending = false;
    TargetBehavior behavior{};
    AttemptEpoch epoch{};
  };

  [[nodiscard]] TargetBehavior behavior_for(const TargetId& target) const;

  const Clock* clock_;
  RuntimeLimits limits_{};
  std::map<Digest256, Effect> effects_;
  std::vector<Digest256> live_;
  std::map<TargetId, TargetBehavior, std::less<>> behaviors_;
  TargetBehavior default_behavior_{};
  AdapterStats stats_{};
  bool available_ = true;
  std::uint32_t drop_evidence_ = 0;
  IEvidenceSink* sink_ = nullptr;
};

class SimHealthAdapter final : public IHealthAdapter {
 public:
  SimHealthAdapter(const Clock& clock, RuntimeLimits limits, HealthSourceId source);

  // The sink the orchestrator registers. Probe answers are delivered here, in
  // the same way the process-backed adapter delivers them.
  void set_sink(IEvidenceSink* sink) noexcept { sink_ = sink; }
  IEvidenceSink* sink() const noexcept { return sink_; }

  [[nodiscard]] std::string_view name() const noexcept override { return "sim-health"; }
  [[nodiscard]] HealthSourceId source_id() const noexcept override { return source_; }
  [[nodiscard]] bool available() const noexcept override { return available_; }
  [[nodiscard]] Status request_probe(const HealthProbeRequest& request) override;
  [[nodiscard]] HealthSnapshotView latest(const std::vector<TargetId>& targets) const override;
  [[nodiscard]] AdapterStats stats() const override { return stats_; }

  // --- test and example controls ---
  void set_healthy(const TargetId& target, bool healthy);
  void set_health_age(const Duration& age) noexcept { health_age_ = age; }
  void set_available(bool available) noexcept { available_ = available; }
  void set_live(bool live) noexcept { live_ = live; }
  void set_source(const HealthSourceId& source) noexcept { source_ = source; }
  void set_fail_with(Status status) noexcept { failure_ = std::move(status); }

 private:
  const Clock* clock_;
  RuntimeLimits limits_{};
  HealthSourceId source_{};
  std::map<TargetId, bool, std::less<>> health_;
  Duration health_age_{};
  Timestamp last_observed_at_{};
  bool available_ = true;
  bool live_ = true;
  Status failure_ = Status::success();
  AdapterStats stats_{};
  IEvidenceSink* sink_ = nullptr;
};

}  // namespace rollout_fabric::sim
