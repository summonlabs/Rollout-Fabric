// Rollout Fabric - the controller daemon.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// One process owns the orchestration state. It binds two loopback listeners:
// one for worker processes that will execute attempts, one for control clients
// (the CLI). Both are served by the same single-threaded reactor as the state
// machine, so there is no shared mutable state inside the controller at all.
//
// A restart is a new incarnation: it claims the journal, reports the epoch it
// claimed, and refuses any command or evidence that names an older one.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rollout_fabric/adapters.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/journal.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/orchestrator.hpp"
#include "rollout_fabric/plan.hpp"
#include "rollout_fabric/process_adapter.hpp"
#include "rollout_fabric/protocol.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/target.hpp"
#include "rollout_fabric/time.hpp"
#include "rollout_fabric/transport.hpp"

namespace rollout_fabric {

struct ControllerOptions {
  RuntimeLimits limits{};
  std::string journal_path;
  std::uint16_t worker_port = 0;   // 0 selects an ephemeral port
  std::uint16_t control_port = 0;
  std::string plan_path;
  std::string inventory_path;
  bool auto_start = false;
  bool auto_arm = false;
  Duration tick_interval = Duration::from_millis(20);
  AuthorityId authority{};
  // When non-zero the controller is responsible for the rollout it created in a
  // previous incarnation rather than only serving commands.
  bool resume_existing = true;
};

struct ControllerStatus {
  std::uint16_t worker_port = 0;
  std::uint16_t control_port = 0;
  IncarnationId incarnation{};
  EpochCounter controller_epoch{0};
  std::uint32_t rollouts = 0;
  std::uint64_t ticks = 0;
  std::uint64_t commands = 0;
  std::uint64_t evidence_accepted = 0;
  std::uint64_t evidence_rejected = 0;
  std::uint64_t workers_attached = 0;
};

class Controller {
 public:
  ~Controller();
  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;

  static Result<std::unique_ptr<Controller>> create(ControllerOptions options);

  // Opens the journal, recovers state, binds both listeners and prints the
  // readiness line. Nothing runs before this returns successfully.
  [[nodiscard]] Status open();
  // One reactor iteration: accept, poll workers, admit evidence, tick the state
  // machine, serve control requests. Never blocks.
  [[nodiscard]] Status step();
  // Steps until a stop is requested.
  [[nodiscard]] Status run();
  void request_stop(std::string reason);

  [[nodiscard]] const ControllerStatus& status() const noexcept { return status_; }
  // Defined in src/controller.cpp: the orchestrator lives inside the controller private
  // implementation, so no installed header exposes its state.
  [[nodiscard]] Orchestrator& orchestrator() noexcept;
  [[nodiscard]] const Orchestrator& orchestrator() const noexcept;
  [[nodiscard]] const RecoveryOutcome& recovery() const noexcept { return recovery_; }
  [[nodiscard]] bool stop_requested() const noexcept { return stop_requested_; }

 private:
  Controller();

  struct Impl;
  std::unique_ptr<Impl> impl_;
  ControllerStatus status_{};
  RecoveryOutcome recovery_{};
  bool stop_requested_ = false;
};

// Blocking command client used by the CLI and by the integration suite. It
// exchanges exactly one request/response pair per connection, which keeps the
// controller's client handling trivial and stateless.
class ControlClient {
 public:
  ControlClient() = default;
  ~ControlClient();
  ControlClient(const ControlClient&) = delete;
  ControlClient& operator=(const ControlClient&) = delete;
  ControlClient(ControlClient&&) noexcept;

  [[nodiscard]] static Result<ControlClient> connect(std::uint16_t port,
                                                     Duration connect_timeout,
                                                     const RuntimeLimits& limits);
  [[nodiscard]] Result<CommandResponseMessage> send(const CommandRequestMessage& request);
  [[nodiscard]] Status close();

 private:
  std::unique_ptr<FrameChannel> channel_;
  RuntimeLimits limits_{};
  std::uint64_t sequence_ = 0;
};

}  // namespace rollout_fabric
