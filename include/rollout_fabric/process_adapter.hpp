// Rollout Fabric - execution and health planes backed by real worker processes.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// One attached worker process is one duplex framed TCP connection. Dispatch is
// a request/ack protocol; completion is a separate, later piece of evidence
// produced by the worker - the two are never conflated, and the adapter cannot
// manufacture a completion.
//
// When a connection dies, every attempt that was dispatched over it becomes
// "unknown" rather than "failed": losing contact is not evidence that an effect
// did not happen. The orchestrator reconciles those attempts against a
// reconnected worker using their stable idempotency keys.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "rollout_fabric/adapters.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"
#include "rollout_fabric/transport.hpp"

namespace rollout_fabric {

struct ProcessAdapterOptions {
  RuntimeLimits limits{};
  Duration connect_timeout = Duration::from_seconds(5);
  // How long an attached worker may be silent before the adapter considers the
  // connection unhealthy.
  Duration silence_timeout = Duration::from_seconds(5);
  std::uint32_t max_workers = 64;
};

class ProcessAdapter final : public IExecutionAdapter, public IHealthAdapter {
 public:
  ProcessAdapter(const Clock& clock, ProcessAdapterOptions options);
  ~ProcessAdapter() override;

  ProcessAdapter(const ProcessAdapter&) = delete;
  ProcessAdapter& operator=(const ProcessAdapter&) = delete;

  // Attaches an accepted connection. The worker announces itself with a Hello;
  // until that arrives the connection is not eligible for dispatch.
  [[nodiscard]] Status attach(Socket socket);

  // --- IExecutionAdapter ---
  [[nodiscard]] std::string_view name() const noexcept override { return "process-execution"; }
  [[nodiscard]] bool available() const noexcept override;
  [[nodiscard]] DispatchResult dispatch(const DispatchRequest& request) override;
  [[nodiscard]] ReconcileResult reconcile(const std::vector<AttemptFence>& attempts) override;
  [[nodiscard]] Status cancel(const AttemptFence& fence) override;
  void poll(IEvidenceSink& sink) override;
  [[nodiscard]] AdapterStats stats() const override { return stats_; }

  // --- IHealthAdapter ---
  [[nodiscard]] HealthSourceId source_id() const noexcept override { return health_source_; }
  [[nodiscard]] Status request_probe(const HealthProbeRequest& request) override;
  [[nodiscard]] HealthSnapshotView latest(const std::vector<TargetId>& targets) const override;

  // --- supervision ---
  void set_health_source(const HealthSourceId& source) noexcept { health_source_ = source; }
  [[nodiscard]] std::vector<WorkerId> attached_workers() const;
  [[nodiscard]] std::size_t attached_count() const noexcept { return connections_.size(); }
  [[nodiscard]] std::uint32_t in_flight_total() const noexcept;
  // Asks every attached worker to stop serving and close.
  [[nodiscard]] Status request_shutdown(std::string reason);
  // Closes every connection immediately, which is what a controller shutdown
  // does after it has flushed its journal. Dispatch becomes impossible.
  void close_all();

  // Registers the sink used for health answers that arrive during poll(). The
  // orchestrator installs itself; the adapter never calls the sink from another
  // thread.
  void set_sink(IEvidenceSink* sink) noexcept { sink_hint_ = sink; }

 private:
  struct Connection;

  [[nodiscard]] Connection* pick_worker(const TargetId& target);
  [[nodiscard]] Status send_to(Connection& connection, const Frame& frame);
  void handle_frame(Connection& connection, const Frame& frame, IEvidenceSink& sink);
  void handle_hello(Connection& connection, const Frame& frame, IEvidenceSink& sink);
  void handle_dispatch_ack(Connection& connection, const Frame& frame);
  void handle_evidence(Connection& connection, const Frame& frame, IEvidenceSink& sink);
  void handle_cancel_ack(Connection& connection, const Frame& frame);
  void handle_reconcile_response(Connection& connection, const Frame& frame, IEvidenceSink& sink);
  void handle_health_response(Connection& connection, const Frame& frame, IEvidenceSink& sink);
  void drop_connection(std::size_t index, std::string detail, IEvidenceSink* sink);
  [[nodiscard]] Sequence next_sequence() noexcept { return Sequence{++frame_sequence_}; }

  const Clock* clock_;
  ProcessAdapterOptions options_{};
  HealthSourceId health_source_{};
  std::vector<std::unique_ptr<Connection>> connections_;
  std::unordered_map<AttemptId, std::size_t, StrongIdHash<AttemptIdTag>> attempt_routes_;
  struct PendingReconcile {
    std::uint64_t request_id = 0;
    std::size_t connection_index = 0;
    std::vector<AttemptFence> fences;
    bool answered = false;
    ReconcileResult result{};
  };
  std::vector<PendingReconcile> pending_reconciles_;
  struct PendingHealth {
    std::uint64_t request_id = 0;
    std::vector<TargetId> targets;
    Timestamp issued_at{};
  };
  std::vector<PendingHealth> pending_health_;
  std::vector<HealthSample> latest_health_;
  Timestamp latest_health_at_{};
  bool latest_health_live_ = false;
  AdapterStats stats_{};
  std::uint64_t frame_sequence_ = 0;
  std::uint64_t next_request_id_ = 0;
  IEvidenceSink* sink_hint_ = nullptr;
};

}  // namespace rollout_fabric
