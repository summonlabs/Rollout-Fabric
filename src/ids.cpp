// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/ids.hpp"

#include <chrono>
#include <cstdint>
#include <string>

#include "rollout_fabric/codec.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace rollout_fabric {
namespace {

[[nodiscard]] Digest256 entropy_seed() noexcept {
  ByteWriter writer;
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  writer.u64(static_cast<std::uint64_t>(nanos));
  const auto steady = std::chrono::steady_clock::now().time_since_epoch();
  writer.u64(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(steady).count()));
#if defined(_WIN32)
  // GetCurrentProcessId rather than the deprecated CRT _getpid.
  writer.u64(static_cast<std::uint64_t>(::GetCurrentProcessId()));
#else
  writer.u64(static_cast<std::uint64_t>(::getpid()));
#endif
  // A stack address is deliberately mixed in: it is not a secret, and it
  // decorrelates identifiers minted by processes that started in the same
  // nanosecond on different hosts.
  const std::uintptr_t stack_marker = reinterpret_cast<std::uintptr_t>(&writer);
  writer.u64(static_cast<std::uint64_t>(stack_marker));
  writer.string("rollout-fabric/entropy/v1");

  // Final mixing so that two processes with equal observable inputs still
  // diverge with overwhelming probability.
  Digest256 seed = sha256(writer.span());
  for (int round = 0; round < 4; ++round) {
    Sha256 hasher;
    hasher.update(std::span<const std::byte>(seed.data(), seed.size()));
    const std::byte round_byte = static_cast<std::byte>(static_cast<std::uint8_t>(round));
    hasher.update(std::span<const std::byte>(&round_byte, 1));
    seed = hasher.finish();
  }
  return seed;
}

}  // namespace

std::string_view to_string(IdDomain domain) noexcept {
  switch (domain) {
    case IdDomain::kRollout: return "rollout";
    case IdDomain::kStage: return "stage";
    case IdDomain::kCohort: return "cohort";
    case IdDomain::kAttempt: return "attempt";
    case IdDomain::kGeneration: return "generation";
    case IdDomain::kPlan: return "plan";
    case IdDomain::kAction: return "action";
    case IdDomain::kArtifact: return "artifact";
    case IdDomain::kEvidence: return "evidence";
    case IdDomain::kDecision: return "decision";
    case IdDomain::kIncarnation: return "incarnation";
    case IdDomain::kAuthority: return "authority";
    case IdDomain::kOperator: return "operator";
    case IdDomain::kWorker: return "worker";
    case IdDomain::kHealthSource: return "health_source";
    case IdDomain::kGate: return "gate";
    case IdDomain::kSnapshot: return "snapshot";
    case IdDomain::kJournal: return "journal";
    case IdDomain::kFailureDomain: return "failure_domain";
    case IdDomain::kVerifier: return "verifier";
  }
  return "unknown";
}

IdFactory::IdFactory() noexcept : IdFactory(entropy_seed()) {}

IdFactory::IdFactory(const Digest256& seed) noexcept : seed_(seed), counter_(0) {}

IdFactory IdFactory::from_entropy() noexcept { return IdFactory(entropy_seed()); }

Digest256 IdFactory::derive(IdDomain domain, std::uint64_t counter) const noexcept {
  ByteWriter writer;
  writer.string("rollout-fabric/id/v1");
  writer.u16(static_cast<std::uint16_t>(domain));
  writer.u64(counter);
  writer.raw(std::span<const std::byte>(seed_.data(), seed_.size()));
  return sha256(writer.span());
}

}  // namespace rollout_fabric
