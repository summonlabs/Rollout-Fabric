// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/policy.hpp"

#include <string>

namespace rollout_fabric {
namespace {

void write_duration(ByteWriter& writer, Duration value) {
  writer.i64(value.nanos());
}

[[nodiscard]] Result<Duration> read_duration(ByteReader& reader) {
  auto value = reader.i64();
  if (!value.ok()) {
    return value.status();
  }
  return Duration::from_nanos(value.value());
}

[[nodiscard]] std::string ratio_is_invalid() {
  return "a ratio requires a non-zero denominator and a numerator no larger than it";
}

[[nodiscard]] Status validate_ratio(const Ratio& value, const char* field) {
  if (value.denominator == 0) {
    return make_status(StatusCode::kInvalidArgument, std::string(field) + ": " + ratio_is_invalid());
  }
  if (value.numerator > value.denominator) {
    return make_status(StatusCode::kInvalidArgument,
                       std::string(field) + ": numerator exceeds denominator (a fraction above one "
                                            "has no meaning for this threshold)");
  }
  return Status::success();
}

}  // namespace

std::string_view to_string(GateMode mode) noexcept {
  switch (mode) {
    case GateMode::kAutomatic: return "automatic";
    case GateMode::kManual: return "manual";
  }
  return "unknown";
}

std::optional<GateMode> parse_gate_mode(std::string_view text) noexcept {
  if (text == "automatic" || text == "auto") {
    return GateMode::kAutomatic;
  }
  if (text == "manual") {
    return GateMode::kManual;
  }
  return std::nullopt;
}

std::string_view to_string(GateKind kind) noexcept {
  switch (kind) {
    case GateKind::kStageEntry: return "stage_entry";
    case GateKind::kEvidence: return "evidence";
    case GateKind::kHealth: return "health";
    case GateKind::kAdvance: return "advance";
    case GateKind::kRollback: return "rollback";
  }
  return "unknown";
}

std::string_view to_string(InFlightPolicy policy) noexcept {
  switch (policy) {
    case InFlightPolicy::kLetFinish: return "let_finish";
    case InFlightPolicy::kCancel: return "cancel";
  }
  return "unknown";
}

std::optional<InFlightPolicy> parse_in_flight_policy(std::string_view text) noexcept {
  if (text == "let_finish" || text == "let-finish") {
    return InFlightPolicy::kLetFinish;
  }
  if (text == "cancel") {
    return InFlightPolicy::kCancel;
  }
  return std::nullopt;
}

Status Ratio::validate() const {
  if (denominator == 0) {
    return make_status(StatusCode::kInvalidArgument, ratio_is_invalid());
  }
  if (numerator > denominator) {
    return make_status(StatusCode::kInvalidArgument, ratio_is_invalid());
  }
  return Status::success();
}

std::uint64_t Ratio::apply_floor(std::uint64_t value) const noexcept {
  if (denominator == 0) {
    return 0;
  }
  if (numerator == 0) {
    return 0;
  }
  if (numerator >= denominator) {
    return value;
  }
  // (value / denominator) * numerator cannot exceed value because
  // numerator <= denominator; the remainder term is bounded by
  // (denominator - 1) * numerator < 2^64 for 32-bit inputs.
  const std::uint64_t quotient = value / denominator;
  const std::uint64_t remainder = value % denominator;
  return quotient * numerator + (remainder * numerator) / denominator;
}

std::uint64_t Ratio::apply_ceil(std::uint64_t value) const noexcept {
  if (denominator == 0 || numerator == 0) {
    return 0;
  }
  if (numerator >= denominator) {
    return value;
  }
  const std::uint64_t floor_value = apply_floor(value);
  const std::uint64_t exact = floor_value * denominator;
  return exact == value * numerator ? floor_value : floor_value + 1;
}

bool Ratio::satisfied_by(std::uint64_t part, std::uint64_t whole) const noexcept {
  if (denominator == 0) {
    return false;
  }
  if (whole == 0) {
    return numerator == 0;
  }
  // part / whole >= numerator / denominator  <=>  part * denominator >= whole * numerator
  return compare_products(part, denominator, whole, numerator) >= 0;
}

int Ratio::compare(const Ratio& other) const noexcept {
  return compare_products(numerator, other.denominator, other.numerator, denominator);
}

std::string Ratio::to_string() const {
  return std::to_string(numerator) + "/" + std::to_string(denominator);
}

Ratio ratio(std::uint32_t numerator, std::uint32_t denominator) noexcept {
  Ratio value;
  value.numerator = numerator;
  value.denominator = denominator;
  return value;
}

Status StagePolicy::validate(const RuntimeLimits& limits) const {
  (void)limits;
  if (max_concurrency == 0) {
    return make_status(StatusCode::kInvalidArgument, "max_concurrency must be at least 1");
  }
  Status status = validate_ratio(min_healthy_fraction, "min_healthy_fraction");
  if (!status.ok()) {
    return status;
  }
  status = validate_ratio(failure_budget_fraction, "failure_budget_fraction");
  if (!status.ok()) {
    return status;
  }
  status = validate_ratio(health_gate.min_healthy_fraction, "health_gate.min_healthy_fraction");
  if (!status.ok()) {
    return status;
  }
  status = validate_ratio(health_gate.failure_budget_fraction, "health_gate.failure_budget_fraction");
  if (!status.ok()) {
    return status;
  }
  status = validate_ratio(evidence_gate.min_passing_fraction, "evidence_gate.min_passing_fraction");
  if (!status.ok()) {
    return status;
  }
  if (attempt_deadline.is_zero() || attempt_deadline.is_negative()) {
    return make_status(StatusCode::kInvalidArgument, "attempt_deadline must be positive");
  }
  if (evidence_max_age.is_zero() || evidence_max_age.is_negative()) {
    return make_status(StatusCode::kInvalidArgument, "evidence_max_age must be positive");
  }
  if (max_attempts_per_target == 0) {
    return make_status(StatusCode::kInvalidArgument, "max_attempts_per_target must be at least 1");
  }
  if (health_gate.enabled && health_gate.max_age.is_zero()) {
    return make_status(StatusCode::kInvalidArgument,
                       "health_gate.max_age must be positive when the health gate is enabled");
  }
  if (health_gate.enabled && health_gate.min_consecutive_healthy == 0) {
    return make_status(StatusCode::kInvalidArgument,
                       "health_gate.min_consecutive_healthy must be at least 1 when the health gate "
                       "is enabled");
  }
  if (evidence_gate.soak.is_negative()) {
    return make_status(StatusCode::kInvalidArgument, "evidence_gate.soak must not be negative");
  }
  if (evidence_gate.max_age.is_zero() || evidence_gate.max_age.is_negative()) {
    return make_status(StatusCode::kInvalidArgument, "evidence_gate.max_age must be positive");
  }
  if (evidence_gate.min_passing_samples == 0) {
    return make_status(StatusCode::kInvalidArgument, "evidence_gate.min_passing_samples must be at least 1");
  }
  if (blast_radius.max_targets_changing_per_stage == 0 ||
      blast_radius.max_targets_changing_global == 0) {
    return make_status(StatusCode::kInvalidArgument,
                       "blast radius limits must be at least 1; an unbounded rollout is not a "
                       "configuration this runtime accepts");
  }
  if (blast_radius.max_targets_changing_per_failure_domain == 0 ||
      blast_radius.max_targets_changing_per_rack == 0 ||
      blast_radius.max_targets_changing_per_pod == 0 ||
      blast_radius.max_targets_changing_per_site == 0 ||
      blast_radius.max_failure_domains_changing == 0) {
    return make_status(StatusCode::kInvalidArgument,
                       "every blast radius dimension must be at least 1");
  }
  if (blast_radius.max_targets_changing_per_stage > blast_radius.max_targets_changing_global) {
    return make_status(StatusCode::kInvalidArgument,
                       "max_targets_changing_per_stage exceeds max_targets_changing_global");
  }
  if (rollback_supported && compensation_actions.empty()) {
    return make_status(StatusCode::kInvalidArgument,
                       "rollback_supported requires at least one compensation action so that a "
                       "rollback has something to run");
  }
  if (!rollback_supported && !compensation_actions.empty()) {
    return make_status(StatusCode::kInvalidArgument,
                       "compensation_actions were declared for a stage that does not support "
                       "rollback; the plan must say which of the two it means");
  }
  if (weight == 0) {
    return make_status(StatusCode::kInvalidArgument, "weight must be at least 1");
  }
  return Status::success();
}

void encode(ByteWriter& writer, const Ratio& value) {
  writer.u32(value.numerator);
  writer.u32(value.denominator);
}

Result<Ratio> decode_ratio(ByteReader& reader) {
  auto numerator = reader.u32();
  if (!numerator.ok()) {
    return numerator.status();
  }
  auto denominator = reader.u32();
  if (!denominator.ok()) {
    return denominator.status();
  }
  Ratio value;
  value.numerator = numerator.value();
  value.denominator = denominator.value();
  const Status status = value.validate();
  if (!status.ok()) {
    return status;
  }
  return value;
}

Digest256 StagePolicy::policy_digest() const {
  ByteWriter writer;
  writer.string("rollout-fabric/stage-policy/v1");
  writer.u32(max_concurrency);
  encode(writer, min_healthy_fraction);
  writer.u32(failure_budget_targets);
  encode(writer, failure_budget_fraction);
  writer.u32(canary_size);
  writer.boolean(canary_requires_health_gate);
  writer.boolean(health_gate.enabled);
  write_duration(writer, health_gate.max_age);
  writer.raw(std::span<const std::byte>(health_gate.required_source.bytes().data(),
                                        health_gate.required_source.bytes().size()));
  writer.u32(health_gate.min_consecutive_healthy);
  encode(writer, health_gate.min_healthy_fraction);
  writer.u32(health_gate.failure_budget_targets);
  encode(writer, health_gate.failure_budget_fraction);
  writer.u8(static_cast<std::uint8_t>(evidence_gate.mode));
  write_duration(writer, evidence_gate.soak);
  write_duration(writer, evidence_gate.max_age);
  writer.raw(std::span<const std::byte>(evidence_gate.required_verifier.bytes().data(),
                                        evidence_gate.required_verifier.bytes().size()));
  writer.u32(evidence_gate.min_passing_samples);
  encode(writer, evidence_gate.min_passing_fraction);
  writer.boolean(evidence_gate.require_full_coverage);
  writer.u8(static_cast<std::uint8_t>(entry_gate));
  writer.u8(static_cast<std::uint8_t>(advance_gate));
  writer.u32(blast_radius.max_targets_changing_global);
  writer.u32(blast_radius.max_targets_changing_per_stage);
  writer.u32(blast_radius.max_targets_changing_per_failure_domain);
  writer.u32(blast_radius.max_targets_changing_per_rack);
  writer.u32(blast_radius.max_targets_changing_per_pod);
  writer.u32(blast_radius.max_targets_changing_per_site);
  writer.u32(blast_radius.max_failure_domains_changing);
  write_duration(writer, attempt_deadline);
  writer.u32(max_attempts_per_target);
  write_duration(writer, evidence_max_age);
  writer.u8(static_cast<std::uint8_t>(pause_in_flight));
  writer.boolean(rollback_supported);
  writer.u32(static_cast<std::uint32_t>(compensation_actions.size()));
  for (const ActionId& action : compensation_actions) {
    writer.raw(std::span<const std::byte>(action.bytes().data(), action.bytes().size()));
  }
  writer.boolean(require_predecessors_succeeded);
  writer.u32(weight);
  return sha256(writer.span());
}

std::string StagePolicy::describe() const {
  std::string text;
  text.append("concurrency=").append(std::to_string(max_concurrency));
  text.append(" min_healthy=").append(min_healthy_fraction.to_string());
  text.append(" failure_budget=").append(std::to_string(failure_budget_targets));
  text.append("+").append(failure_budget_fraction.to_string());
  text.append(" canary=").append(canary_size == 0 ? "none" : std::to_string(canary_size));
  text.append(" entry_gate=").append(to_string(entry_gate));
  text.append(" advance_gate=").append(to_string(advance_gate));
  text.append(" soak_ms=").append(std::to_string(evidence_gate.soak.millis()));
  text.append(" pause_in_flight=").append(to_string(pause_in_flight));
  text.append(" rollback=").append(rollback_supported ? "supported" : "unsupported");
  return text;
}

void encode(ByteWriter& writer, const StagePolicy& value) {
  writer.u32(value.max_concurrency);
  encode(writer, value.min_healthy_fraction);
  writer.u32(value.failure_budget_targets);
  encode(writer, value.failure_budget_fraction);
  writer.u32(value.canary_size);
  writer.boolean(value.canary_requires_health_gate);
  writer.boolean(value.health_gate.enabled);
  write_duration(writer, value.health_gate.max_age);
  writer.raw(value.health_gate.required_source.span());
  writer.u32(value.health_gate.min_consecutive_healthy);
  encode(writer, value.health_gate.min_healthy_fraction);
  writer.u32(value.health_gate.failure_budget_targets);
  encode(writer, value.health_gate.failure_budget_fraction);
  writer.u8(static_cast<std::uint8_t>(value.evidence_gate.mode));
  write_duration(writer, value.evidence_gate.soak);
  write_duration(writer, value.evidence_gate.max_age);
  writer.raw(value.evidence_gate.required_verifier.span());
  writer.u32(value.evidence_gate.min_passing_samples);
  encode(writer, value.evidence_gate.min_passing_fraction);
  writer.boolean(value.evidence_gate.require_full_coverage);
  writer.u8(static_cast<std::uint8_t>(value.entry_gate));
  writer.u8(static_cast<std::uint8_t>(value.advance_gate));
  writer.u32(value.blast_radius.max_targets_changing_global);
  writer.u32(value.blast_radius.max_targets_changing_per_stage);
  writer.u32(value.blast_radius.max_targets_changing_per_failure_domain);
  writer.u32(value.blast_radius.max_targets_changing_per_rack);
  writer.u32(value.blast_radius.max_targets_changing_per_pod);
  writer.u32(value.blast_radius.max_targets_changing_per_site);
  writer.u32(value.blast_radius.max_failure_domains_changing);
  write_duration(writer, value.attempt_deadline);
  writer.u32(value.max_attempts_per_target);
  write_duration(writer, value.evidence_max_age);
  writer.u8(static_cast<std::uint8_t>(value.pause_in_flight));
  writer.boolean(value.rollback_supported);
  writer.u32(static_cast<std::uint32_t>(value.compensation_actions.size()));
  for (const ActionId& action : value.compensation_actions) {
    writer.raw(action.span());
  }
  writer.boolean(value.require_predecessors_succeeded);
  writer.u32(value.weight);
}

Result<StagePolicy> decode_stage_policy(ByteReader& reader, const RuntimeLimits& limits) {
  StagePolicy value;
  auto u32_field = [&reader](std::uint32_t& target) -> Status {
    auto read = reader.u32();
    if (!read.ok()) {
      return read.status();
    }
    target = read.value();
    return Status::success();
  };
  auto bool_field = [&reader](bool& target) -> Status {
    auto read = reader.boolean();
    if (!read.ok()) {
      return read.status();
    }
    target = read.value();
    return Status::success();
  };
  auto u8_field = [&reader](std::uint8_t& target) -> Status {
    auto read = reader.u8();
    if (!read.ok()) {
      return read.status();
    }
    target = read.value();
    return Status::success();
  };

  Status status = u32_field(value.max_concurrency);
  if (!status.ok()) {
    return status;
  }
  auto healthy = decode_ratio(reader);
  if (!healthy.ok()) {
    return healthy.status();
  }
  value.min_healthy_fraction = healthy.value();
  status = u32_field(value.failure_budget_targets);
  if (!status.ok()) {
    return status;
  }
  auto budget = decode_ratio(reader);
  if (!budget.ok()) {
    return budget.status();
  }
  value.failure_budget_fraction = budget.value();
  status = u32_field(value.canary_size);
  if (!status.ok()) {
    return status;
  }
  status = bool_field(value.canary_requires_health_gate);
  if (!status.ok()) {
    return status;
  }
  status = bool_field(value.health_gate.enabled);
  if (!status.ok()) {
    return status;
  }
  auto health_age = read_duration(reader);
  if (!health_age.ok()) {
    return health_age.status();
  }
  value.health_gate.max_age = health_age.value();
  auto health_source = reader.raw(HealthSourceId::byte_size);
  if (!health_source.ok()) {
    return health_source.status();
  }
  value.health_gate.required_source = HealthSourceId::from_bytes(
      std::span<const std::byte, HealthSourceId::byte_size>(health_source.value().data(),
                                                            HealthSourceId::byte_size));
  status = u32_field(value.health_gate.min_consecutive_healthy);
  if (!status.ok()) {
    return status;
  }
  auto health_fraction = decode_ratio(reader);
  if (!health_fraction.ok()) {
    return health_fraction.status();
  }
  value.health_gate.min_healthy_fraction = health_fraction.value();
  status = u32_field(value.health_gate.failure_budget_targets);
  if (!status.ok()) {
    return status;
  }
  auto health_budget = decode_ratio(reader);
  if (!health_budget.ok()) {
    return health_budget.status();
  }
  value.health_gate.failure_budget_fraction = health_budget.value();

  std::uint8_t mode = 0;
  status = u8_field(mode);
  if (!status.ok()) {
    return status;
  }
  if (mode > static_cast<std::uint8_t>(GateMode::kManual)) {
    return make_status(StatusCode::kCorrupt, "unknown evidence gate mode");
  }
  value.evidence_gate.mode = static_cast<GateMode>(mode);
  auto soak = read_duration(reader);
  if (!soak.ok()) {
    return soak.status();
  }
  value.evidence_gate.soak = soak.value();
  auto evidence_age = read_duration(reader);
  if (!evidence_age.ok()) {
    return evidence_age.status();
  }
  value.evidence_gate.max_age = evidence_age.value();
  auto verifier = reader.raw(VerifierId::byte_size);
  if (!verifier.ok()) {
    return verifier.status();
  }
  value.evidence_gate.required_verifier = VerifierId::from_bytes(
      std::span<const std::byte, VerifierId::byte_size>(verifier.value().data(),
                                                        VerifierId::byte_size));
  status = u32_field(value.evidence_gate.min_passing_samples);
  if (!status.ok()) {
    return status;
  }
  auto passing = decode_ratio(reader);
  if (!passing.ok()) {
    return passing.status();
  }
  value.evidence_gate.min_passing_fraction = passing.value();
  status = bool_field(value.evidence_gate.require_full_coverage);
  if (!status.ok()) {
    return status;
  }

  std::uint8_t entry = 0;
  status = u8_field(entry);
  if (!status.ok()) {
    return status;
  }
  if (entry > static_cast<std::uint8_t>(GateMode::kManual)) {
    return make_status(StatusCode::kCorrupt, "unknown entry gate mode");
  }
  value.entry_gate = static_cast<GateMode>(entry);
  std::uint8_t advance = 0;
  status = u8_field(advance);
  if (!status.ok()) {
    return status;
  }
  if (advance > static_cast<std::uint8_t>(GateMode::kManual)) {
    return make_status(StatusCode::kCorrupt, "unknown advance gate mode");
  }
  value.advance_gate = static_cast<GateMode>(advance);

  status = u32_field(value.blast_radius.max_targets_changing_global);
  if (!status.ok()) {
    return status;
  }
  status = u32_field(value.blast_radius.max_targets_changing_per_stage);
  if (!status.ok()) {
    return status;
  }
  status = u32_field(value.blast_radius.max_targets_changing_per_failure_domain);
  if (!status.ok()) {
    return status;
  }
  status = u32_field(value.blast_radius.max_targets_changing_per_rack);
  if (!status.ok()) {
    return status;
  }
  status = u32_field(value.blast_radius.max_targets_changing_per_pod);
  if (!status.ok()) {
    return status;
  }
  status = u32_field(value.blast_radius.max_targets_changing_per_site);
  if (!status.ok()) {
    return status;
  }
  status = u32_field(value.blast_radius.max_failure_domains_changing);
  if (!status.ok()) {
    return status;
  }
  auto deadline = read_duration(reader);
  if (!deadline.ok()) {
    return deadline.status();
  }
  value.attempt_deadline = deadline.value();
  status = u32_field(value.max_attempts_per_target);
  if (!status.ok()) {
    return status;
  }
  auto max_age = read_duration(reader);
  if (!max_age.ok()) {
    return max_age.status();
  }
  value.evidence_max_age = max_age.value();

  std::uint8_t in_flight = 0;
  status = u8_field(in_flight);
  if (!status.ok()) {
    return status;
  }
  if (in_flight > static_cast<std::uint8_t>(InFlightPolicy::kCancel)) {
    return make_status(StatusCode::kCorrupt, "unknown in-flight policy");
  }
  value.pause_in_flight = static_cast<InFlightPolicy>(in_flight);

  status = bool_field(value.rollback_supported);
  if (!status.ok()) {
    return status;
  }
  std::uint32_t action_count = 0;
  status = u32_field(action_count);
  if (!status.ok()) {
    return status;
  }
  if (action_count > limits.max_actions_per_stage) {
    return make_status(StatusCode::kLimitExceeded,
                       "compensation action count " + std::to_string(action_count) +
                           " exceeds max_actions_per_stage");
  }
  value.compensation_actions.reserve(action_count);
  for (std::uint32_t index = 0; index < action_count; ++index) {
    auto raw = reader.raw(ActionId::byte_size);
    if (!raw.ok()) {
      return raw.status();
    }
    value.compensation_actions.push_back(ActionId::from_bytes(
        std::span<const std::byte, ActionId::byte_size>(raw.value().data(), ActionId::byte_size)));
  }
  status = bool_field(value.require_predecessors_succeeded);
  if (!status.ok()) {
    return status;
  }
  status = u32_field(value.weight);
  if (!status.ok()) {
    return status;
  }
  status = value.validate(limits);
  if (!status.ok()) {
    return status;
  }
  return value;
}

}  // namespace rollout_fabric
