// Rollout Fabric - durable state inspection.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Offline inspection of a journal, a plan document or an inventory document.
// It opens the journal read-only and never takes the writer lock, so it can be
// pointed at a running controller's journal without disturbing it.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "rollout_fabric/json.hpp"
#include "rollout_fabric/journal.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/persistence.hpp"
#include "rollout_fabric/plan.hpp"
#include "rollout_fabric/report.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/target.hpp"
#include "rollout_fabric/version.hpp"

namespace {

[[nodiscard]] bool read_file(const std::string& path, std::string& out) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return true;
}

void print_usage() {
  std::printf(
      "rf-inspect - Rollout Fabric inspection tool\n"
      "\n"
      "usage: rf-inspect <subject> --path <file> [options]\n"
      "\n"
      "subjects:\n"
      "  journal     replay a journal, report its integrity and summarise its records\n"
      "  plan        parse and validate an approved Change Planner plan document\n"
      "  inventory   parse and validate a target inventory document\n"
      "\n"
      "options:\n"
      "  --path <file>       document to inspect (required)\n"
      "  --limit <n>         show at most n journal records (default: 20)\n"
      "  --digests           print the payload digest of every shown record\n"
      "  --help              print this text\n"
      "\n"
      "The journal is opened read-only and without the writer lock.\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string subject;
  std::string path;
  std::uint32_t limit = 20;
  bool digests = false;
  bool help = false;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      help = true;
    } else if (argument == "--path" && index + 1 < argc) {
      path = argv[++index];
    } else if (argument == "--limit" && index + 1 < argc) {
      limit = static_cast<std::uint32_t>(std::strtoul(argv[++index], nullptr, 10));
    } else if (argument == "--digests") {
      digests = true;
    } else if (!argument.empty() && argument[0] == '-') {
      std::fprintf(stderr, "rf-inspect: unknown argument '%s'\n", argument.c_str());
      return 2;
    } else if (subject.empty()) {
      subject = argument;
    } else {
      std::fprintf(stderr, "rf-inspect: exactly one subject is required\n");
      return 2;
    }
  }
  if (help) {
    print_usage();
    return 0;
  }
  if (subject.empty() || path.empty()) {
    print_usage();
    return 2;
  }

  const rollout_fabric::RuntimeLimits limits;
  if (subject == "plan") {
    std::string text;
    if (!read_file(path, text)) {
      std::fprintf(stderr, "rf-inspect: cannot read '%s'\n", path.c_str());
      return 1;
    }
    auto plan = rollout_fabric::parse_change_plan(text, limits);
    if (!plan.ok()) {
      std::fprintf(stderr, "rf-inspect: %s\n", plan.status().to_string().c_str());
      return 1;
    }
    const rollout_fabric::ChangePlan& value = plan.value();
    std::printf("plan %s\n", value.id.to_hex().c_str());
    std::printf("  change_id=%s approved_by=%s approved_at=%s\n", value.change_id.c_str(),
                value.approved_by.c_str(), value.approved_at.to_rfc3339().c_str());
    std::printf("  source_digest=%s\n", rollout_fabric::to_hex(value.source_digest).c_str());
    std::printf("  plan_digest=%s\n", rollout_fabric::to_hex(value.plan_digest()).c_str());
    for (const rollout_fabric::StageId& stage_id : value.execution_order()) {
      const rollout_fabric::StageSpec* stage = value.find_stage(stage_id);
      if (stage == nullptr) {
        continue;
      }
      std::printf("  stage %s name=%s ordinal=%u predecessors=%zu actions=%zu\n",
                  stage->id.to_hex().substr(0, 12).c_str(), stage->name.c_str(), stage->ordinal,
                  stage->predecessors.size(), stage->actions.size());
      std::printf("    selector: %s\n", stage->selector.describe().c_str());
      std::printf("    policy:   %s\n", stage->policy.describe().c_str());
      std::printf("    stage_digest=%s\n",
                  rollout_fabric::to_hex(stage->stage_digest()).substr(0, 32).c_str());
    }
    return 0;
  }

  if (subject == "inventory") {
    std::string text;
    if (!read_file(path, text)) {
      std::fprintf(stderr, "rf-inspect: cannot read '%s'\n", path.c_str());
      return 1;
    }
    auto inventory = rollout_fabric::parse_inventory(text, limits);
    if (!inventory.ok()) {
      std::fprintf(stderr, "rf-inspect: %s\n", inventory.status().to_string().c_str());
      return 1;
    }
    std::printf("inventory targets=%zu digest=%s epoch=%llu\n", inventory.value().size(),
                rollout_fabric::to_hex(inventory.value().inventory_digest()).c_str(),
                static_cast<unsigned long long>(
                    rollout_fabric::value_of(inventory.value().epoch())));
    for (const rollout_fabric::TargetDescriptor& descriptor : inventory.value().all()) {
      std::printf("  %s  %s\n", descriptor.id.to_hex().substr(0, 12).c_str(),
                  rollout_fabric::render_target_brief(descriptor).c_str());
    }
    return 0;
  }

  if (subject != "journal") {
    std::fprintf(stderr, "rf-inspect: unknown subject '%s'\n", subject.c_str());
    print_usage();
    return 2;
  }

  std::vector<rollout_fabric::JournalRecord> records;
  auto report = rollout_fabric::scan_journal(path, limits, &records, false);
  if (!report.ok()) {
    std::fprintf(stderr, "rf-inspect: %s\n", report.status().to_string().c_str());
    return 1;
  }
  std::printf("%s\n", report.value().render().c_str());
  const std::size_t show = records.size() < limit ? records.size() : limit;
  for (std::size_t index = 0; index < show; ++index) {
    const rollout_fabric::JournalRecord& record = records[index];
    std::printf("  [%llu] %-22s payload=%zu epoch=%llu at=%s\n",
                static_cast<unsigned long long>(rollout_fabric::value_of(record.sequence)),
                std::string(rollout_fabric::to_string(record.type)).c_str(), record.payload.size(),
                static_cast<unsigned long long>(
                    rollout_fabric::value_of(record.controller_epoch)),
                record.written_at.to_rfc3339().c_str());
    if (digests) {
      std::printf("        digest=%s\n", rollout_fabric::to_hex(record.payload_digest).c_str());
    }
    if (record.type == rollout_fabric::JournalRecordType::kRolloutSnapshot) {
      auto rollout = rollout_fabric::decode_rollout_payload(record.payload, limits);
      if (!rollout.ok()) {
        std::printf("        snapshot rejected: %s\n", rollout.status().to_string().c_str());
        continue;
      }
      std::printf("        rollout=%s state=%s generation=%s revision=%llu stages=%zu\n",
                  rollout.value().id.to_hex().substr(0, 12).c_str(),
                  std::string(rollout_fabric::to_string(rollout.value().state)).c_str(),
                  rollout.value().generation.to_hex().substr(0, 12).c_str(),
                  static_cast<unsigned long long>(
                      rollout_fabric::value_of(rollout.value().revision)),
                  rollout.value().stages.size());
    }
  }
  if (records.size() > show) {
    std::printf("  ... %zu more record(s)\n", records.size() - show);
  }
  return 0;
}
