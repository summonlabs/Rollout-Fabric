// Rollout Fabric - the target worker process.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A worker is a real process that owns real threads. It connects to the
// controller, accepts dispatch, performs the effect on a worker thread, and
// reports evidence - acceptance immediately, completion only when the effect is
// actually finished.
//
// Threading contract: the reactor thread owns the socket and the effect table.
// Worker threads own one effect each and communicate through a mutex-guarded
// completion queue. The mutex is held only long enough to move a value into the
// queue: no socket call, no callback and no allocation that can fail beneath
// the lock. The reactor drains the queue between socket operations and never
// holds the lock while doing IO.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"
#include "rollout_fabric/transport.hpp"

namespace rollout_fabric {

// Behaviour of the simulated effect a worker performs. The worker does not
// touch a device: it is a real process with real threads and real sockets whose
// effect is a bounded computation. Substituting a device-facing executor means
// implementing the same message contract.
struct WorkerTargetScenario {
  TargetId target{};
  bool matches_all = false;
  Duration work_duration = Duration::from_millis(50);
  bool fail = false;
  bool rollback_fails = false;
  bool never_complete = false;
  bool duplicate_report = false;
  bool reorder_report = false;
  bool stale_health = false;
  bool always_unhealthy = false;
  bool crash_on_target = false;
  Duration health_age = Duration::from_seconds(0);
};

struct WorkerScenario {
  WorkerTargetScenario fallback{};
  std::vector<WorkerTargetScenario> targets;

  [[nodiscard]] static Result<WorkerScenario> parse(std::string_view json_text, const RuntimeLimits& limits);
  [[nodiscard]] static Result<WorkerScenario> load(const std::string& path, const RuntimeLimits& limits);
  [[nodiscard]] const WorkerTargetScenario& for_target(const TargetId& target) const noexcept;
};

struct WorkerOptions {
  RuntimeLimits limits{};
  std::string controller_host = "127.0.0.1";
  std::uint16_t controller_port = 0;
  std::string name;
  std::uint32_t capacity = 4;
  WorkerScenario scenario{};
  Duration connect_timeout = Duration::from_seconds(5);
  Duration heartbeat_interval = Duration::from_millis(250);
  // When true the process exits after serving this many attempts. Used by the
  // failure-injection suite to model a worker that stops being available.
  std::uint64_t exit_after_attempts = 0;
};

class Worker {
 public:
  ~Worker();
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  static Result<std::unique_ptr<Worker>> create(WorkerOptions options);

  // Connects, announces itself and serves until the controller shuts it down or
  // the controller disappears.
  [[nodiscard]] Status run();

  [[nodiscard]] const WorkerId& id() const noexcept { return worker_id_; }
  [[nodiscard]] const IncarnationId& incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] std::string_view ready_line() const noexcept { return ready_line_; }

 private:
  Worker();

  struct Impl;
  std::unique_ptr<Impl> impl_;
  WorkerId worker_id_{};
  IncarnationId incarnation_{};
  std::string ready_line_;
};

}  // namespace rollout_fabric
