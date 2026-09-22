// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Real-process proof. Nothing in this file is a thread pretending to be a
// process: a controller daemon and two target workers are started as separate
// operating system processes, they speak the framed protocol over loopback TCP,
// and the suite kills the daemon mid-stage to prove that progress survives a
// restart. The same documents the CLI uses are the documents loaded here.
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "rollout_fabric/controller.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/plan.hpp"
#include "rollout_fabric/process.hpp"
#include "rollout_fabric/protocol.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"
#include "test_harness.hpp"

namespace {

using namespace rollout_fabric;

std::string g_rolloutd;
std::string g_worker;
std::string g_workdir;

[[nodiscard]] std::string join(const std::string& directory, const std::string& name) {
  if (directory.empty()) {
    return name;
  }
  const char last = directory.back();
  if (last == '\\' || last == '/') {
    return directory + name;
  }
  return directory + "\\" + name;
}

[[nodiscard]] bool write_file(const std::string& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.close();
  return static_cast<bool>(output);
}

[[nodiscard]] std::string plan_document() {
  return std::string(R"JSON({
  "kind": "rollout_fabric.change_plan",
  "schema_version": 1,
  "change_id": "CHG-PROCESS-TEST",
  "approved_by": "change-planner",
  "approved_at": "2026-01-01T00:00:00Z",
  "source_digest": "3b1f4c2a9e8d7065b4a3928176f5e4d3c2b1a09f8e7d6c5b4a3928176f5e4d3c",
  "stages": [
    {
      "name": "wave-one",
      "ordinal": 0,
      "selector": { "op": "and", "children": [ { "op": "by_kind", "kinds": ["switch"] },
                                                { "op": "by_site", "sites": ["site-a"] } ] },
      "policy": {
        "max_concurrency": 2, "min_healthy_fraction": "1/1", "failure_budget_targets": 0,
        "canary_size": 1, "canary_requires_health_gate": true,
        "entry_gate": "automatic", "advance_gate": "automatic",
        "attempt_deadline_ms": 30000, "max_attempts_per_target": 2, "evidence_max_age_ms": 60000,
        "health_gate": { "enabled": true, "max_age_ms": 30000, "min_consecutive_healthy": 1,
                         "min_healthy_fraction": "1/1" },
        "evidence_gate": { "mode": "automatic", "soak_ms": 100, "max_age_ms": 60000,
                           "require_full_coverage": false },
        "blast_radius": { "max_targets_changing_global": 2, "max_targets_changing_per_stage": 2,
                          "max_targets_changing_per_failure_domain": 1,
                          "max_targets_changing_per_rack": 1, "max_targets_changing_per_pod": 1,
                          "max_targets_changing_per_site": 2, "max_failure_domains_changing": 2 }
      },
      "evidence_rule": { "success_authority": "execution_adapter_attested",
                         "failure_authority": "execution_adapter_attested" },
      "actions": [ { "name": "apply", "kind": "apply_configuration",
                     "artifact_digest": "aa11bb22cc33dd44ee55ff6600778899aabbccddeeff00112233445566778899" } ]
    },
    {
      "name": "wave-two",
      "ordinal": 1,
      "predecessors": ["wave-one"],
      "selector": { "op": "and", "children": [ { "op": "by_kind", "kinds": ["switch"] },
                                                { "op": "by_site", "sites": ["site-b"] } ] },
      "policy": {
        "max_concurrency": 2, "min_healthy_fraction": "1/1", "failure_budget_targets": 0,
        "canary_size": 0, "entry_gate": "manual", "advance_gate": "automatic",
        "attempt_deadline_ms": 30000, "max_attempts_per_target": 2, "evidence_max_age_ms": 60000,
        "health_gate": { "enabled": true, "max_age_ms": 30000, "min_consecutive_healthy": 1,
                         "min_healthy_fraction": "1/1" },
        "evidence_gate": { "mode": "automatic", "soak_ms": 100, "max_age_ms": 60000,
                           "require_full_coverage": false },
        "blast_radius": { "max_targets_changing_global": 4, "max_targets_changing_per_stage": 4,
                          "max_targets_changing_per_failure_domain": 1,
                          "max_targets_changing_per_rack": 1, "max_targets_changing_per_pod": 2,
                          "max_targets_changing_per_site": 4, "max_failure_domains_changing": 3 }
      },
      "evidence_rule": { "success_authority": "execution_adapter_attested",
                         "failure_authority": "execution_adapter_attested" },
      "actions": [ { "name": "apply", "kind": "apply_configuration",
                     "artifact_digest": "aa11bb22cc33dd44ee55ff6600778899aabbccddeeff00112233445566778899" } ]
    }
  ]
})JSON");
}

[[nodiscard]] std::string inventory_document() {
  std::string text =
      "{\n  \"kind\": \"rollout_fabric.inventory\",\n  \"schema_version\": 1,\n  \"targets\": [\n";
  for (int index = 0; index < 8; ++index) {
    text += "    { \"name\": \"leaf-0" + std::to_string(index) + "\", \"kind\": \"switch\", \"site\": \"" +
            (index < 4 ? "site-a" : "site-b") + "\", \"pod\": \"pod-" + std::to_string(index % 4) +
            "\", \"rack\": \"rack-" + std::to_string(index) + "\", \"failure_domain\": \"fd-" +
            std::to_string(index % 4) + "\" }";
    text += index == 7 ? "\n" : ",\n";
  }
  text += "  ]\n}\n";
  return text;
}

struct Daemon {
  ChildProcess process{};
  std::uint16_t control_port = 0;
  std::uint16_t worker_port = 0;
  std::string journal;
};

// Starts a controller and blocks until it prints its readiness line. There is
// no timeout here: a daemon that never becomes ready is a defect to diagnose,
// not something to paper over with a watchdog.
[[nodiscard]] bool start_daemon(Daemon& daemon, const std::string& journal, const std::string& plan,
                                const std::string& inventory, const std::string& extra,
                                std::string& detail) {
  ProcessSpawnOptions options;
  options.executable = g_rolloutd;
  options.working_directory = g_workdir;
  options.capture_stdout = true;
  options.arguments = {"--journal", journal, "--plan", plan, "--inventory", inventory, "--auto-arm",
                       "--auto-start", "--tick-ms", "20"};
  if (!extra.empty()) {
    options.arguments.push_back(extra);
  }
  auto spawned = ChildProcess::spawn(options);
  if (!spawned.ok()) {
    detail = "cannot start the controller: " + spawned.status().to_string();
    return false;
  }
  daemon.process = std::move(spawned).value();
  daemon.journal = journal;
  auto line = daemon.process.read_stdout_line();
  if (!line.ok()) {
    detail = "the controller never reported readiness";
    return false;
  }
  const std::string text = line.value();
  const auto extract = [&text](const char* key) -> std::uint16_t {
    const std::size_t at = text.find(key);
    if (at == std::string::npos) {
      return 0;
    }
    return static_cast<std::uint16_t>(std::strtoul(text.c_str() + at + std::strlen(key), nullptr, 10));
  };
  daemon.control_port = extract("control_port=");
  daemon.worker_port = extract("worker_port=");
  if (daemon.control_port == 0 || daemon.worker_port == 0) {
    detail = "the readiness line did not name both ports: " + text;
    return false;
  }
  return true;
}

[[nodiscard]] bool start_worker(ChildProcess& worker, std::uint16_t port, const std::string& name,
                                std::string& detail) {
  ProcessSpawnOptions options;
  options.executable = g_worker;
  options.working_directory = g_workdir;
  options.capture_stdout = true;
  options.arguments = {"--controller-port", std::to_string(port), "--name", name, "--capacity", "4"};
  auto spawned = ChildProcess::spawn(options);
  if (!spawned.ok()) {
    detail = "cannot start a worker: " + spawned.status().to_string();
    return false;
  }
  worker = std::move(spawned).value();
  auto line = worker.read_stdout_line();
  if (!line.ok()) {
    detail = "the worker never reported readiness";
    return false;
  }
  return true;
}

[[nodiscard]] CommandResponseMessage command(std::uint16_t port, CommandKind kind,
                                             const RolloutId& rollout, const GateId& gate,
                                             std::string reason, std::string& detail) {
  CommandResponseMessage response;
  auto client = ControlClient::connect(port, Duration::from_seconds(5), default_limits());
  if (!client.ok()) {
    detail = "cannot reach the controller: " + client.status().to_string();
    response.code = StatusCode::kUnavailable;
    return response;
  }
  CommandRequestMessage request;
  request.kind = kind;
  request.rollout = rollout;
  request.gate = gate;
  request.reason = std::move(reason);
  request.operator_name = "process-suite";
  auto answer = client.value().send(request);
  if (!answer.ok()) {
    detail = answer.status().to_string();
    response.code = StatusCode::kUnavailable;
    return response;
  }
  return answer.value();
}

// Polls the controller with read-only status commands. This is a readiness
// wait, not a test timeout: the loop is bounded by the tick budget and a
// rollout that never progresses fails the assertion that follows it.
// Polls status. A nil rollout asks for the listing first, learns the identifier
// from it, and then polls that rollout specifically: the listing carries the
// state but not the pending gate, and an operator needs the gate.
[[nodiscard]] bool wait_for_state(std::uint16_t port, RolloutId& rollout,
                                  const std::vector<std::string>& wanted, std::uint32_t budget,
                                  std::string& state, GateId& gate) {
  for (std::uint32_t attempt = 0; attempt < budget; ++attempt) {
    std::string detail;
    const CommandResponseMessage response =
        command(port, CommandKind::kStatus, rollout, GateId{}, std::string{}, detail);
    if (response.code == StatusCode::kOk) {
      if (rollout.is_nil()) {
        const std::string hex = response.payload.substr(0, 32);
        auto parsed = RolloutId::parse(hex);
        if (parsed.has_value()) {
          rollout = *parsed;
          continue;
        }
      }
      const std::size_t at = response.payload.find("state=");
      if (at != std::string::npos) {
        const std::size_t end = response.payload.find_first_of(" \n", at);
        state = response.payload.substr(at + 6, end - at - 6);
      }
      const std::size_t gate_at = response.payload.find("pending_gate=");
      if (gate_at != std::string::npos) {
        const std::string hex =
            response.payload.substr(gate_at + 13, 32);
        auto parsed = GateId::parse(hex);
        if (parsed.has_value()) {
          gate = *parsed;
        }
      }
    }
    for (const std::string& candidate : wanted) {
      if (state == candidate) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

}  // namespace

RF_TEST(multiprocess_rollout_completes_and_survives_a_controller_kill) {
  RF_REQUIRE(!g_rolloutd.empty());
  RF_REQUIRE(!g_worker.empty());

  const std::string plan_path = join(g_workdir, "process-plan.json");
  const std::string inventory_path = join(g_workdir, "process-inventory.json");
  const std::string journal = join(g_workdir, "process.journal");
  RF_CHECK(write_file(plan_path, plan_document()));
  RF_CHECK(write_file(inventory_path, inventory_document()));

  std::string detail;
  Daemon daemon;
  RF_REQUIRE(start_daemon(daemon, "process.journal", "process-plan.json", "process-inventory.json",
                          std::string{}, detail));

  ChildProcess worker_one;
  ChildProcess worker_two;
  RF_REQUIRE(start_worker(worker_one, daemon.worker_port, "w1", detail));
  RF_REQUIRE(start_worker(worker_two, daemon.worker_port, "w2", detail));

  // The rollout is created by the daemon from the plan it was given.
  std::string state;
  GateId gate;
  RolloutId rollout{};
  RF_REQUIRE(wait_for_state(daemon.control_port, rollout, {"running", "gated", "completed"},
                            200, state, gate));
  RF_CHECK(state == "running" || state == "gated" || state == "completed");

  // Kill the controller mid-stage. The workers keep running; the journal keeps
  // the progress.
  RF_CHECK(daemon.process.terminate().ok());
  const auto waited = daemon.process.wait_for(Duration::from_seconds(10));
  RF_CHECK(waited.ok());
  RF_CHECK(worker_one.running());
  RF_CHECK(worker_two.running());

  // A second controller adopts the same journal as a new incarnation.
  Daemon restarted;
  RF_REQUIRE(start_daemon(restarted, "process.journal", "process-plan.json",
                          "process-inventory.json", std::string{}, detail));
  RF_CHECK(restarted.control_port != daemon.control_port);

  // Workers must reconnect to the new incarnation, because the controller that
  // owned their sockets is gone.
  ChildProcess worker_three;
  RF_REQUIRE(start_worker(worker_three, restarted.worker_port, "w3", detail));

  state.clear();
  gate = GateId{};
  RF_REQUIRE(wait_for_state(restarted.control_port, rollout, {"running", "gated", "completed"},
                            600, state, gate));
  // Drive the manual gate when the second stage reaches it, then let the
  // rollout finish. The gate identifier is read from the status output, exactly
  // as an operator would.
  for (int attempt = 0; attempt < 900; ++attempt) {
    if (state == "completed" || state == "failed") {
      break;
    }
    if (state == "gated" && !gate.is_nil()) {
      std::string gate_detail;
      const CommandResponseMessage response = command(restarted.control_port,
                                                      CommandKind::kApproveGate, rollout, gate,
                                                      "approved by the process suite", gate_detail);
      RF_CHECK(response.code == StatusCode::kOk);
      gate = GateId{};
    }
    std::string ignored;
    GateId found;
    (void)wait_for_state(restarted.control_port, rollout, std::vector<std::string>{}, 1, state,
                         found);
    if (!found.is_nil()) {
      gate = found;
    }
  }
  RF_CHECK_EQ(state, std::string("completed"));

  // Tidy up: the suite must not leak processes into the environment.
  (void)worker_one.terminate();
  (void)worker_two.terminate();
  (void)worker_three.terminate();
  (void)restarted.process.terminate();
  (void)worker_one.wait_for(Duration::from_seconds(10));
  (void)worker_two.wait_for(Duration::from_seconds(10));
  (void)worker_three.wait_for(Duration::from_seconds(10));
  (void)restarted.process.wait_for(Duration::from_seconds(10));
}

int main(int argc, char** argv) {
  std::string filter;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--rolloutd" && index + 1 < argc) {
      g_rolloutd = argv[++index];
    } else if (argument == "--worker" && index + 1 < argc) {
      g_worker = argv[++index];
    } else if (argument == "--cli" && index + 1 < argc) {
      ++index;  // accepted for symmetry with the other suites
    } else if (argument == "--inspect" && index + 1 < argc) {
      ++index;
    } else if (argument == "--workdir" && index + 1 < argc) {
      g_workdir = argv[++index];
    } else if (argument != "process") {
      filter = argument;
    }
  }
  if (g_workdir.empty()) {
    auto directory = current_executable_directory();
    if (directory.ok()) {
      g_workdir = directory.value();
    }
  }
  if (!filter.empty()) {
    rf_test::Registry::instance().set_filter(filter);
  }
  if (g_rolloutd.empty() || g_worker.empty()) {
    std::printf("process suite skipped: --rolloutd and --worker are required\n");
    return 0;
  }
  return rf_test::Registry::instance().run_all();
}
