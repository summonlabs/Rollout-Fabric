// Rollout Fabric - controller daemon.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// rolloutd owns the orchestration state for one journal. It binds a worker
// listener and a control listener, prints exactly one readiness line on stdout
// once it is open for business, and then runs its reactor until it is stopped.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "rollout_fabric/controller.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/version.hpp"

namespace {

struct Options {
  rollout_fabric::ControllerOptions controller{};
  bool help = false;
};

void print_usage() {
  std::printf(
      "rolloutd - Rollout Fabric controller\n"
      "\n"
      "usage: rolloutd --journal <path> [options]\n"
      "\n"
      "  --journal <path>        durable journal file (required; a .lock sidecar is created)\n"
      "  --worker-port <n>       loopback port for worker processes (default: ephemeral)\n"
      "  --control-port <n>      loopback port for control clients (default: ephemeral)\n"
      "  --plan <path>           approved Change Planner plan to create a rollout from\n"
      "  --inventory <path>      target inventory document\n"
      "  --auto-arm              arm the created rollout immediately\n"
      "  --auto-start            start the created rollout immediately (implies --auto-arm)\n"
      "  --tick-ms <n>           reactor tick interval (default: 20)\n"
      "  --max-targets <n>       override max_total_targets\n"
      "  --help                  print this text\n"
      "\n"
      "The daemon prints one readiness line on stdout and flushes it:\n"
      "  rolloutd ready incarnation=<hex> epoch=<n> control_port=<n> worker_port=<n> pid=<n>\n"
      "\n"
      "The control and worker listeners are loopback-only and carry no message\n"
      "authentication; they are a trusted local path, not a network API.\n");
}

[[nodiscard]] bool parse_u32(const char* text, std::uint32_t& out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  std::uint64_t value = 0;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') {
      return false;
    }
    value = value * 10u + static_cast<std::uint64_t>(*cursor - '0');
    if (value > 0xFFFFFFFFull) {
      return false;
    }
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

[[nodiscard]] bool parse_u16(const char* text, std::uint16_t& out) {
  std::uint32_t value = 0;
  if (!parse_u32(text, value) || value > 65535u) {
    return false;
  }
  out = static_cast<std::uint16_t>(value);
  return true;
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options, std::string& error) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto next = [&](const char* name) -> const char* {
      if (index + 1 >= argc) {
        error = std::string(name) + " requires a value";
        return nullptr;
      }
      ++index;
      return argv[index];
    };
    if (argument == "--help" || argument == "-h") {
      options.help = true;
      return true;
    }
    if (argument == "--journal") {
      const char* value = next("--journal");
      if (value == nullptr) {
        return false;
      }
      options.controller.journal_path = value;
    } else if (argument == "--worker-port") {
      const char* value = next("--worker-port");
      if (value == nullptr || !parse_u16(value, options.controller.worker_port)) {
        error = "--worker-port requires a port number";
        return false;
      }
    } else if (argument == "--control-port") {
      const char* value = next("--control-port");
      if (value == nullptr || !parse_u16(value, options.controller.control_port)) {
        error = "--control-port requires a port number";
        return false;
      }
    } else if (argument == "--plan") {
      const char* value = next("--plan");
      if (value == nullptr) {
        return false;
      }
      options.controller.plan_path = value;
    } else if (argument == "--inventory") {
      const char* value = next("--inventory");
      if (value == nullptr) {
        return false;
      }
      options.controller.inventory_path = value;
    } else if (argument == "--auto-arm") {
      options.controller.auto_arm = true;
    } else if (argument == "--auto-start") {
      options.controller.auto_start = true;
      options.controller.auto_arm = true;
    } else if (argument == "--tick-ms") {
      const char* value = next("--tick-ms");
      std::uint32_t millis = 0;
      if (value == nullptr || !parse_u32(value, millis) || millis == 0) {
        error = "--tick-ms requires a positive number of milliseconds";
        return false;
      }
      options.controller.tick_interval = rollout_fabric::Duration::from_millis(millis);
    } else if (argument == "--max-targets") {
      const char* value = next("--max-targets");
      std::uint32_t count = 0;
      if (value == nullptr || !parse_u32(value, count) || count == 0) {
        error = "--max-targets requires a positive number of targets";
        return false;
      }
      options.controller.limits.max_total_targets = count;
      options.controller.limits.max_targets_per_cohort = count;
    } else {
      error = "unknown argument '" + argument + "'";
      return false;
    }
  }
  if (!options.help && options.controller.journal_path.empty()) {
    error = "--journal is required";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!parse_options(argc, argv, options, error)) {
    std::fprintf(stderr, "rolloutd: %s\n", error.c_str());
    print_usage();
    return 2;
  }
  if (options.help) {
    print_usage();
    return 0;
  }

  const rollout_fabric::Status limits_status = options.controller.limits.validate();
  if (!limits_status.ok()) {
    std::fprintf(stderr, "rolloutd: invalid limits: %s\n", limits_status.to_string().c_str());
    return 2;
  }

  auto controller = rollout_fabric::Controller::create(std::move(options.controller));
  if (!controller.ok()) {
    std::fprintf(stderr, "rolloutd: %s\n", controller.status().to_string().c_str());
    return 1;
  }
  rollout_fabric::Controller& instance = *controller.value();
  const rollout_fabric::Status opened = instance.open();
  if (!opened.ok()) {
    std::fprintf(stderr, "rolloutd: %s\n", opened.to_string().c_str());
    return 1;
  }
  const rollout_fabric::Status ran = instance.run();
  if (!ran.ok()) {
    std::fprintf(stderr, "rolloutd: %s\n", ran.to_string().c_str());
    return 1;
  }
  return 0;
}
