// Rollout Fabric - target worker process.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// One worker process serves one controller over one framed TCP connection. The
// worker performs bounded computation on real threads; it does not touch a
// device. Replacing it with a device-facing executor means implementing the
// same message contract, not changing the orchestration runtime.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/worker.hpp"

namespace {

struct Options {
  rollout_fabric::WorkerOptions worker{};
  bool help = false;
};

void print_usage() {
  std::printf(
      "rf-worker - Rollout Fabric target worker\n"
      "\n"
      "usage: rf-worker --controller-port <n> [options]\n"
      "\n"
      "  --controller-port <n>   loopback port the controller listens on (required)\n"
      "  --controller-host <h>   loopback host (default: 127.0.0.1)\n"
      "  --name <text>           worker name reported in the handshake\n"
      "  --capacity <n>          attempts this worker will accept at once (default: 4)\n"
      "  --scenario <path>       JSON fault-injection scenario for simulated targets\n"
      "  --exit-after <n>        exit after serving n attempts (0 = never)\n"
      "  --max-attempts <n>      override max_concurrent_attempts\n"
      "  --help                  print this text\n"
      "\n"
      "The worker prints one readiness line on stdout before it connects:\n"
      "  rf-worker id=<hex> incarnation=<hex> controller_port=<n> capacity=<n>\n"
      "\n"
      "SYNTHETIC SURFACE: the effect a worker performs is a bounded computation,\n"
      "not a change to a switch or a device. Process boundaries, threads, sockets,\n"
      "framing, fencing, cancellation and failure injection are real.\n");
}

[[nodiscard]] bool parse_u64(const char* text, std::uint64_t& out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  std::uint64_t value = 0;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') {
      return false;
    }
    value = value * 10u + static_cast<std::uint64_t>(*cursor - '0');
  }
  out = value;
  return true;
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options, std::string& error) {
  bool have_port = false;
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
    if (argument == "--controller-port") {
      const char* value = next("--controller-port");
      std::uint64_t port = 0;
      if (value == nullptr || !parse_u64(value, port) || port == 0 || port > 65535u) {
        error = "--controller-port requires a port number";
        return false;
      }
      options.worker.controller_port = static_cast<std::uint16_t>(port);
      have_port = true;
    } else if (argument == "--controller-host") {
      const char* value = next("--controller-host");
      if (value == nullptr) {
        return false;
      }
      options.worker.controller_host = value;
    } else if (argument == "--name") {
      const char* value = next("--name");
      if (value == nullptr) {
        return false;
      }
      options.worker.name = value;
    } else if (argument == "--capacity") {
      const char* value = next("--capacity");
      std::uint64_t capacity = 0;
      if (value == nullptr || !parse_u64(value, capacity) || capacity == 0 ||
          capacity > 65535u) {
        error = "--capacity requires a positive number";
        return false;
      }
      options.worker.capacity = static_cast<std::uint32_t>(capacity);
    } else if (argument == "--scenario") {
      const char* value = next("--scenario");
      if (value == nullptr) {
        return false;
      }
      auto scenario = rollout_fabric::WorkerScenario::load(value, options.worker.limits);
      if (!scenario.ok()) {
        error = "cannot load scenario '" + std::string(value) + "': " +
                scenario.status().to_string();
        return false;
      }
      options.worker.scenario = std::move(scenario).value();
    } else if (argument == "--exit-after") {
      const char* value = next("--exit-after");
      std::uint64_t count = 0;
      if (value == nullptr || !parse_u64(value, count)) {
        error = "--exit-after requires a number";
        return false;
      }
      options.worker.exit_after_attempts = count;
    } else if (argument == "--max-attempts") {
      const char* value = next("--max-attempts");
      std::uint64_t count = 0;
      if (value == nullptr || !parse_u64(value, count) || count == 0) {
        error = "--max-attempts requires a positive number";
        return false;
      }
      options.worker.limits.max_concurrent_attempts = static_cast<std::uint32_t>(count);
    } else {
      error = "unknown argument '" + argument + "'";
      return false;
    }
  }
  if (!options.help && !have_port) {
    error = "--controller-port is required";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!parse_options(argc, argv, options, error)) {
    std::fprintf(stderr, "rf-worker: %s\n", error.c_str());
    print_usage();
    return 2;
  }
  if (options.help) {
    print_usage();
    return 0;
  }
  auto worker = rollout_fabric::Worker::create(std::move(options.worker));
  if (!worker.ok()) {
    std::fprintf(stderr, "rf-worker: %s\n", worker.status().to_string().c_str());
    return 1;
  }
  rollout_fabric::Worker& instance = *worker.value();
  const rollout_fabric::Status ran = instance.run();
  if (!ran.ok() && ran.code() != rollout_fabric::StatusCode::kUnavailable &&
      ran.code() != rollout_fabric::StatusCode::kCancelled &&
      ran.code() != rollout_fabric::StatusCode::kShuttingDown) {
    std::fprintf(stderr, "rf-worker: %s\n", ran.to_string().c_str());
    return 1;
  }
  return 0;
}
