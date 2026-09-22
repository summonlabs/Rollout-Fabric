// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/persistence.hpp"

#include <string>

#include "decode_util.hpp"

namespace rollout_fabric {
namespace {

using detail::dc_id;
using detail::dc_string;
using detail::dc_u32;
using detail::wr_id;

[[nodiscard]] std::vector<std::byte> wrap(std::string_view tag,
                                          std::span<const std::byte> body) {
  ByteWriter writer;
  writer.string(tag);
  writer.u32(kPayloadSchemaVersion);
  writer.bytes(body);
  return std::vector<std::byte>(writer.span().begin(), writer.span().end());
}

[[nodiscard]] Result<std::span<const std::byte>> unwrap(std::string_view expected,
                                                        std::span<const std::byte> bytes,
                                                        const RuntimeLimits& limits) {
  ByteReader reader(bytes);
  auto tag = reader.string(64);
  if (!tag.ok()) {
    return tag.status();
  }
  if (tag.value() != expected) {
    return make_status(StatusCode::kCorrupt,
                       "payload tag '" + tag.value() + "' is not '" + std::string(expected) + "'");
  }
  auto schema = reader.u32();
  if (!schema.ok()) {
    return schema.status();
  }
  if (schema.value() != kPayloadSchemaVersion) {
    return make_status(StatusCode::kNotSupported,
                       "payload schema version " + std::to_string(schema.value()) +
                           " is not readable by this build");
  }
  auto body = reader.bytes(limits.max_journal_record_bytes);
  if (!body.ok()) {
    return body.status();
  }
  const Status trailing = reader.expect_end();
  if (!trailing.ok()) {
    return trailing;
  }
  return body.value();
}

}  // namespace

std::vector<std::byte> encode_inventory_payload(const TargetInventory& inventory,
                                                const RuntimeLimits& limits) {
  (void)limits;
  ByteWriter body;
  body.raw(std::span<const std::byte>(inventory.inventory_digest().data(), kDigest256Bytes));
  body.u32(static_cast<std::uint32_t>(inventory.size()));
  for (const TargetDescriptor& descriptor : inventory.all()) {
    encode(body, descriptor);
  }
  return wrap("rollout-fabric/inventory/v1", body.span());
}

Result<TargetInventory> decode_inventory_payload(std::span<const std::byte> bytes,
                                                 const RuntimeLimits& limits) {
  auto body = unwrap("rollout-fabric/inventory/v1", bytes, limits);
  if (!body.ok()) {
    return body.status();
  }
  ByteReader reader(body.value());
  Digest256 expected{};
  RF_TRY(detail::dc_digest(reader, "inventory.digest", expected));
  std::uint32_t count = 0;
  RF_TRY(dc_u32(reader, "inventory.count", count));
  if (count > limits.max_total_targets) {
    return make_status(StatusCode::kLimitExceeded, "inventory payload declares too many targets");
  }
  TargetInventory inventory;
  for (std::uint32_t index = 0; index < count; ++index) {
    auto descriptor = decode_target_descriptor(reader, limits);
    if (!descriptor.ok()) {
      return descriptor.status();
    }
    const Status status = inventory.add(std::move(descriptor).value(), limits);
    if (!status.ok()) {
      return status;
    }
  }
  RF_TRY(reader.expect_end());
  if (!digest_equal(expected, inventory.inventory_digest())) {
    return make_status(StatusCode::kCorrupt,
                       "the inventory payload does not match the digest recorded with it");
  }
  return inventory;
}

std::vector<std::byte> encode_plan_payload(const ChangePlan& plan, const RuntimeLimits& limits) {
  (void)limits;
  ByteWriter body;
  body.raw(std::span<const std::byte>(plan.plan_digest().data(), kDigest256Bytes));
  encode(body, plan);
  return wrap("rollout-fabric/plan/v1", body.span());
}

Result<ChangePlan> decode_plan_payload(std::span<const std::byte> bytes, const RuntimeLimits& limits) {
  auto body = unwrap("rollout-fabric/plan/v1", bytes, limits);
  if (!body.ok()) {
    return body.status();
  }
  ByteReader reader(body.value());
  Digest256 expected{};
  RF_TRY(detail::dc_digest(reader, "plan.digest", expected));
  auto plan = decode_plan(reader, limits);
  if (!plan.ok()) {
    return plan.status();
  }
  RF_TRY(reader.expect_end());
  if (!digest_equal(expected, plan.value().plan_digest())) {
    return make_status(StatusCode::kCorrupt,
                       "the plan payload does not match the digest recorded with it");
  }
  return plan;
}

std::vector<std::byte> encode_rollout_payload(const Rollout& rollout, const RuntimeLimits& limits) {
  (void)limits;
  ByteWriter body;
  body.raw(std::span<const std::byte>(rollout_digest(rollout).data(), kDigest256Bytes));
  encode(body, rollout);
  return wrap("rollout-fabric/rollout/v1", body.span());
}

Result<Rollout> decode_rollout_payload(std::span<const std::byte> bytes, const RuntimeLimits& limits) {
  auto body = unwrap("rollout-fabric/rollout/v1", bytes, limits);
  if (!body.ok()) {
    return body.status();
  }
  ByteReader reader(body.value());
  Digest256 expected{};
  RF_TRY(detail::dc_digest(reader, "rollout.digest", expected));
  auto rollout = decode_rollout(reader, limits);
  if (!rollout.ok()) {
    return rollout.status();
  }
  RF_TRY(reader.expect_end());
  if (!digest_equal(expected, rollout_digest(rollout.value()))) {
    return make_status(StatusCode::kCorrupt,
                       "the rollout snapshot does not match the digest recorded with it");
  }
  return rollout;
}

std::vector<std::byte> encode_operator_command(const OperatorCommandRecord& record,
                                               const RuntimeLimits& limits) {
  ByteWriter body;
  wr_id(body, record.rollout);
  wr_id(body, record.generation);
  body.u64(value_of(record.revision_observed));
  wr_id(body, record.operator_id);
  wr_id(body, record.authority);
  wr_id(body, record.incarnation);
  body.u64(value_of(record.controller_epoch));
  body.string(record.command);
  body.string(record.reason);
  body.i64(record.issued_at.unix_nanos());
  (void)limits;
  return wrap("rollout-fabric/operator-command/v1", body.span());
}

Result<OperatorCommandRecord> decode_operator_command(std::span<const std::byte> bytes,
                                                      const RuntimeLimits& limits) {
  auto body = unwrap("rollout-fabric/operator-command/v1", bytes, limits);
  if (!body.ok()) {
    return body.status();
  }
  ByteReader reader(body.value());
  OperatorCommandRecord record;
  RF_TRY(dc_id(reader, "command.rollout", record.rollout));
  RF_TRY(dc_id(reader, "command.generation", record.generation));
  std::uint64_t revision = 0;
  RF_TRY(detail::dc_u64(reader, "command.revision", revision));
  record.revision_observed = Revision{revision};
  RF_TRY(dc_id(reader, "command.operator", record.operator_id));
  RF_TRY(dc_id(reader, "command.authority", record.authority));
  RF_TRY(dc_id(reader, "command.incarnation", record.incarnation));
  std::uint64_t epoch = 0;
  RF_TRY(detail::dc_u64(reader, "command.controller_epoch", epoch));
  record.controller_epoch = EpochCounter{epoch};
  RF_TRY(dc_string(reader, "command.command", limits.max_name_bytes, record.command));
  RF_TRY(dc_string(reader, "command.reason", limits.max_description_bytes, record.reason));
  RF_TRY(detail::dc_timestamp(reader, "command.issued_at", record.issued_at));
  RF_TRY(reader.expect_end());
  return record;
}

std::vector<std::byte> encode_incarnation_claim(const IncarnationId& incarnation, EpochCounter epoch,
                                                Timestamp claimed_at) {
  ByteWriter body;
  wr_id(body, incarnation);
  body.u64(value_of(epoch));
  body.i64(claimed_at.unix_nanos());
  return wrap("rollout-fabric/incarnation-claim/v1", body.span());
}

}  // namespace rollout_fabric
