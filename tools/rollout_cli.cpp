// Rollout Fabric - operator command line.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The CLI is a thin, stateless client of a running controller: one connection,
// one command, one answer. It holds no orchestration state of its own, so an
// operator can never observe a stale local view.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "rollout_fabric/controller.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/json.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/plan.hpp"
#include "rollout_fabric/protocol.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"
#include "rollout_fabric/version.hpp"

namespace {

using rollout_fabric::CommandKind;
using rollout_fabric::CommandRequestMessage;
using rollout_fabric::CommandResponseMessage;
using rollout_fabric::Duration;
using rollout_fabric::GateId;
using rollout_fabric::GenerationId;
using rollout_fabric::Revision;
using rollout_fabric::RolloutId;
using rollout_fabric::RuntimeLimits;
using rollout_fabric::StageId;
using rollout_fabric::Status;
using rollout_fabric::StatusCode;

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::string command;
  std::string plan_path;
  std::string inventory_path;
  std::string rollout_hex;
  std::string gate_hex;
  std::string stage_argument;
  std::string decision_hex;
  std::string reason;
  std::string operator_name = "operator";
  std::string expect_generation_hex;
  std::string expect_revision_text;
  std::uint32_t connect_timeout_ms = 5000;
  bool help = false;
};

void print_usage() {
  std::printf(
      "rollout-cli - Rollout Fabric operator command line\n"
      "\n"
      "usage: rollout-cli --port <n> <command> [options]\n"
      "\n"
      "commands:\n"
      "  create      --plan <path>                 create a rollout from an approved plan\n"
      "  arm         --rollout <hex>               freeze cohorts and arm the rollout\n"
      "  start       --rollout <hex>               begin the first stage\n"
      "  pause       --rollout <hex> [--reason s]  stop admitting new work\n"
      "  resume      --rollout <hex>               return to the interrupted state\n"
      "  abort       --rollout <hex> [--reason s]  stop future stages; compensate if declared\n"
      "  approve     --rollout <hex> --gate <hex>  approve a pending manual gate (single use)\n"
      "  retire      --rollout <hex>               move a terminal rollout to retired\n"
      "  regenerate  --rollout <hex>               create a new rollout generation\n"
      "  status      [--rollout <hex>]             show state, stages and counters\n"
      "  explain     --rollout <hex> [--decision <hex>]  show why the rollout is where it is\n"
      "  stages      --rollout <hex> --stage <hex|name> [--plan <path>]  per-target detail\n"
      "  cohort      --rollout <hex> --stage <hex|name> [--plan <path>]  frozen membership\n"
      "  events      list recent adapter events\n"
      "\n"
      "common options:\n"
      "  --host <h>                controller host (default: 127.0.0.1)\n"
      "  --port <n>                controller control port (required)\n"
      "  --operator <name>         name recorded with the command\n"
      "  --expect-generation <hex> refuse the command unless the rollout is at this generation\n"
      "  --expect-revision <n>     refuse the command unless the rollout is at this revision\n"
      "  --connect-timeout-ms <n>  connect and response timeout (default: 5000)\n"
      "  --help                    print this text\n");
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
  std::vector<std::string> positional;
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
    if (argument == "--host") {
      const char* value = next("--host");
      if (value == nullptr) return false;
      options.host = value;
    } else if (argument == "--port") {
      const char* value = next("--port");
      std::uint64_t port = 0;
      if (value == nullptr || !parse_u64(value, port) || port == 0 || port > 65535u) {
        error = "--port requires a port number";
        return false;
      }
      options.port = static_cast<std::uint16_t>(port);
    } else if (argument == "--plan") {
      const char* value = next("--plan");
      if (value == nullptr) return false;
      options.plan_path = value;
    } else if (argument == "--inventory") {
      const char* value = next("--inventory");
      if (value == nullptr) return false;
      options.inventory_path = value;
    } else if (argument == "--rollout") {
      const char* value = next("--rollout");
      if (value == nullptr) return false;
      options.rollout_hex = value;
    } else if (argument == "--gate") {
      const char* value = next("--gate");
      if (value == nullptr) return false;
      options.gate_hex = value;
    } else if (argument == "--stage") {
      const char* value = next("--stage");
      if (value == nullptr) return false;
      options.stage_argument = value;
    } else if (argument == "--decision") {
      const char* value = next("--decision");
      if (value == nullptr) return false;
      options.decision_hex = value;
    } else if (argument == "--reason") {
      const char* value = next("--reason");
      if (value == nullptr) return false;
      options.reason = value;
    } else if (argument == "--operator") {
      const char* value = next("--operator");
      if (value == nullptr) return false;
      options.operator_name = value;
    } else if (argument == "--expect-generation") {
      const char* value = next("--expect-generation");
      if (value == nullptr) return false;
      options.expect_generation_hex = value;
    } else if (argument == "--expect-revision") {
      const char* value = next("--expect-revision");
      if (value == nullptr) return false;
      options.expect_revision_text = value;
    } else if (argument == "--connect-timeout-ms") {
      const char* value = next("--connect-timeout-ms");
      std::uint64_t millis = 0;
      if (value == nullptr || !parse_u64(value, millis) || millis == 0) {
        error = "--connect-timeout-ms requires a positive number";
        return false;
      }
      options.connect_timeout_ms = static_cast<std::uint32_t>(millis);
    } else if (!argument.empty() && argument[0] == '-') {
      error = "unknown argument '" + argument + "'";
      return false;
    } else {
      positional.push_back(argument);
    }
  }
  if (options.help) {
    return true;
  }
  if (positional.size() != 1) {
    error = "exactly one command is required";
    return false;
  }
  options.command = positional.front();
  if (options.port == 0) {
    error = "--port is required";
    return false;
  }
  return true;
}

[[nodiscard]] bool is_hex_id(const std::string& text) {
  if (text.size() != 32) {
    return false;
  }
  for (const char character : text) {
    const bool digit = character >= '0' && character <= '9';
    const bool lower = character >= 'a' && character <= 'f';
    const bool upper = character >= 'A' && character <= 'F';
    if (!digit && !lower && !upper) {
      return false;
    }
  }
  return true;
}

// Turns a stage argument into a StageId. A 32 character hex string is taken as
// the identifier itself; anything else is treated as a stage name and resolved
// against the plan document, which derives exactly the same identifier the
// controller derived.
[[nodiscard]] bool resolve_stage(const Options& options, StageId& out, std::string& error) {
  if (options.stage_argument.empty()) {
    error = "--stage is required for this command";
    return false;
  }
  if (is_hex_id(options.stage_argument)) {
    auto parsed = StageId::parse(options.stage_argument);
    if (!parsed.has_value()) {
      error = "--stage is not a valid stage identifier";
      return false;
    }
    out = *parsed;
    return true;
  }
  if (options.plan_path.empty()) {
    error = "a stage name requires --plan so that the identifier can be derived";
    return false;
  }
  std::ifstream input(options.plan_path, std::ios::binary);
  if (!input) {
    error = "cannot open the plan '" + options.plan_path + "'";
    return false;
  }
  const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  RuntimeLimits limits;
  auto plan = rollout_fabric::parse_change_plan(text, limits);
  if (!plan.ok()) {
    error = "cannot parse the plan: " + plan.status().to_string();
    return false;
  }
  StageId derived{};
  bool found = false;
  for (const rollout_fabric::StageSpec& stage : plan.value().stages) {
    if (stage.name == options.stage_argument) {
      derived = stage.id;
      found = true;
      break;
    }
  }
  if (!found) {
    error = "the plan declares no stage named '" + options.stage_argument + "'";
    return false;
  }
  out = derived;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!parse_options(argc, argv, options, error)) {
    std::fprintf(stderr, "rollout-cli: %s\n", error.c_str());
    print_usage();
    return 2;
  }
  if (options.help) {
    print_usage();
    return 0;
  }

  RuntimeLimits limits;
  CommandRequestMessage request;
  request.operator_name = options.operator_name;
  request.reason = options.reason;

  const auto require_rollout = [&]() -> bool {
    if (options.rollout_hex.empty()) {
      error = "--rollout is required for this command";
      return false;
    }
    auto parsed = RolloutId::parse(options.rollout_hex);
    if (!parsed.has_value()) {
      error = "--rollout is not a valid rollout identifier";
      return false;
    }
    request.rollout = *parsed;
    return true;
  };

  if (options.command == "create") {
    request.kind = CommandKind::kCreate;
    if (options.plan_path.empty()) {
      std::fprintf(stderr, "rollout-cli: --plan is required for create\n");
      return 2;
    }
    request.plan_path = options.plan_path;
  } else if (options.command == "arm") {
    request.kind = CommandKind::kArm;
    if (!require_rollout()) { std::fprintf(stderr, "rollout-cli: %s\n", error.c_str()); return 2; }
  } else if (options.command == "start") {
    request.kind = CommandKind::kStart;
    if (!require_rollout()) { std::fprintf(stderr, "rollout-cli: %s\n", error.c_str()); return 2; }
  } else if (options.command == "pause") {
    request.kind = CommandKind::kPause;
    if (!require_rollout()) { std::fprintf(stderr, "rollout-cli: %s\n", error.c_str()); return 2; }
  } else if (options.command == "resume") {
    request.kind = CommandKind::kResume;
    if (!require_rollout()) { std::fprintf(stderr, "rollout-cli: %s\n", error.c_str()); return 2; }
  } else if (options.command == "abort") {
    request.kind = CommandKind::kAbort;
    if (!require_rollout()) { std::fprintf(stderr, "rollout-cli: %s\n", error.c_str()); return 2; }
  } else if (options.command == "retire") {
    request.kind = CommandKind::kRetire;
    if (!require_rollout()) { std::fprintf(stderr, "rollout-cli: %s\n", error.c_str()); return 2; }
  } else if (options.command == "regenerate") {
    request.kind = CommandKind::kRegenerate;
    if (!require_rollout()) { std::fprintf(stderr, "rollout-cli: %s\n", error.c_str()); return 2; }
  } else if (options.command == "approve") {
    request.kind = CommandKind::kApproveGate;
    if (!require_rollout()) { std::fprintf(stderr, "rollout-cli: %s\n", error.c_str()); return 2; }
    if (options.gate_hex.empty()) {
      std::fprintf(stderr, "rollout-cli: --gate is required for approve\n");
      return 2;
    }
    auto gate = GateId::parse(options.gate_hex);
    if (!gate.has_value()) {
      std::fprintf(stderr, "rollout-cli: --gate is not a valid gate identifier\n");
      return 2;
    }
    request.gate = *gate;
  } else if (options.command == "status") {
    request.kind = CommandKind::kStatus;
    if (!options.rollout_hex.empty()) {
      auto parsed = RolloutId::parse(options.rollout_hex);
      if (!parsed.has_value()) {
        std::fprintf(stderr, "rollout-cli: --rollout is not a valid rollout identifier\n");
        return 2;
      }
      request.rollout = *parsed;
    }
  } else if (options.command == "explain") {
    request.kind = CommandKind::kExplain;
    if (!require_rollout()) { std::fprintf(stderr, "rollout-cli: %s\n", error.c_str()); return 2; }
    if (!options.decision_hex.empty()) {
      auto decision = rollout_fabric::DecisionId::parse(options.decision_hex);
      if (!decision.has_value()) {
        std::fprintf(stderr, "rollout-cli: --decision is not a valid decision identifier\n");
        return 2;
      }
      // The request carries one 128-bit identifier argument whose meaning is
      // fixed by the command kind: a GateId for approve, a StageId for the
      // inspection commands, a DecisionId here.
      request.gate = GateId::from_bytes(std::span<const std::byte, GateId::byte_size>(
          decision->bytes().data(), GateId::byte_size));
    }
  } else if (options.command == "stages" || options.command == "cohort") {
    request.kind = options.command == "stages" ? CommandKind::kInspectStage : CommandKind::kInspectCohort;
    if (!require_rollout()) { std::fprintf(stderr, "rollout-cli: %s\n", error.c_str()); return 2; }
    StageId stage{};
    if (!resolve_stage(options, stage, error)) {
      std::fprintf(stderr, "rollout-cli: %s\n", error.c_str());
      return 2;
    }
    request.gate = GateId::from_bytes(std::span<const std::byte, GateId::byte_size>(
        stage.bytes().data(), GateId::byte_size));
  } else if (options.command == "events") {
    request.kind = CommandKind::kListEvents;
  } else {
    std::fprintf(stderr, "rollout-cli: unknown command '%s'\n", options.command.c_str());
    print_usage();
    return 2;
  }

  if (!options.expect_generation_hex.empty()) {
    auto generation = GenerationId::parse(options.expect_generation_hex);
    if (!generation.has_value()) {
      std::fprintf(stderr, "rollout-cli: --expect-generation is not a valid generation identifier\n");
      return 2;
    }
    request.generation = *generation;
    request.checked_generation = true;
  }
  if (!options.expect_revision_text.empty()) {
    std::uint64_t revision = 0;
    if (!parse_u64(options.expect_revision_text.c_str(), revision)) {
      std::fprintf(stderr, "rollout-cli: --expect-revision requires a number\n");
      return 2;
    }
    request.revision = Revision{revision};
    request.checked_revision = true;
  }

  auto client = rollout_fabric::ControlClient::connect(
      options.port, Duration::from_millis(options.connect_timeout_ms), limits);
  if (!client.ok()) {
    std::fprintf(stderr, "rollout-cli: cannot reach the controller: %s\n",
                 client.status().to_string().c_str());
    return 1;
  }
  auto response = client.value().send(request);
  if (!response.ok()) {
    std::fprintf(stderr, "rollout-cli: %s\n", response.status().to_string().c_str());
    return 1;
  }
  const CommandResponseMessage& message = response.value();
  if (!message.payload.empty()) {
    std::fwrite(message.payload.data(), 1, message.payload.size(), stdout);
    if (message.payload.back() != '\n') {
      std::fputc('\n', stdout);
    }
  }
  if (message.code != StatusCode::kOk) {
    std::fprintf(stderr, "rollout-cli: %s: %s\n",
                 std::string(rollout_fabric::to_string(message.code)).c_str(), message.detail.c_str());
    return 1;
  }
  if (!message.detail.empty() && message.payload.empty()) {
    std::printf("%s\n", message.detail.c_str());
  }
  return 0;
}
