// Rollout Fabric - strongly typed domain identities.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every identity in the runtime is a distinct C++ type. An AttemptId and a
// TargetId are both 128-bit values, but they are not interchangeable and the
// compiler enforces that. Identifiers are derived, never parsed from operator
// input in a way that could collide across domains: the factory hashes a
// domain tag into the value.
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "rollout_fabric/digest.hpp"

namespace rollout_fabric {

// A fixed width opaque identifier parameterised by a tag type.
template <class Tag, std::size_t N = kDigest128Bytes>
class StrongId {
 public:
  using tag_type = Tag;
  static constexpr std::size_t byte_size = N;
  static constexpr std::size_t hex_size = N * 2;

  constexpr StrongId() noexcept = default;

  [[nodiscard]] static StrongId from_bytes(std::span<const std::byte, N> in) noexcept {
    StrongId id;
    for (std::size_t i = 0; i < N; ++i) {
      id.bytes_[i] = in[i];
    }
    return id;
  }

  [[nodiscard]] static std::optional<StrongId> parse(std::string_view hex) noexcept {
    StrongId id;
    if (!parse_hex(hex, std::span<std::byte>(id.bytes_.data(), N))) {
      return std::nullopt;
    }
    return id;
  }

  [[nodiscard]] std::string to_hex() const { return rollout_fabric::to_hex(bytes_); }

  [[nodiscard]] constexpr bool is_nil() const noexcept {
    for (std::size_t i = 0; i < N; ++i) {
      if (bytes_[i] != std::byte{0}) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] constexpr const std::array<std::byte, N>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::span<const std::byte, N> span() const noexcept { return bytes_; }

  friend constexpr auto operator<=>(const StrongId&, const StrongId&) noexcept = default;

 private:
  std::array<std::byte, N> bytes_{};
};

// ---------------------------------------------------------------------------
// Tag types. One per domain object; never reused across domains.
// ---------------------------------------------------------------------------

struct RolloutIdTag {};
struct StageIdTag {};
struct CohortIdTag {};
struct TargetIdTag {};
struct AttemptIdTag {};
struct GenerationIdTag {};
struct PlanIdTag {};
struct ActionIdTag {};
struct ArtifactIdTag {};
struct EvidenceIdTag {};
struct DecisionIdTag {};
struct IncarnationIdTag {};
struct AuthorityIdTag {};
struct OperatorIdTag {};
struct WorkerIdTag {};
struct HealthSourceIdTag {};
struct GateIdTag {};
struct SnapshotIdTag {};
struct JournalIdTag {};
struct FailureDomainIdTag {};
struct VerifierIdTag {};

using RolloutId = StrongId<RolloutIdTag>;
using StageId = StrongId<StageIdTag>;
using CohortId = StrongId<CohortIdTag>;
using TargetId = StrongId<TargetIdTag>;
using AttemptId = StrongId<AttemptIdTag>;
using GenerationId = StrongId<GenerationIdTag>;
using PlanId = StrongId<PlanIdTag>;
using ActionId = StrongId<ActionIdTag>;
using ArtifactId = StrongId<ArtifactIdTag>;
using EvidenceId = StrongId<EvidenceIdTag>;
using DecisionId = StrongId<DecisionIdTag>;
using IncarnationId = StrongId<IncarnationIdTag>;
using AuthorityId = StrongId<AuthorityIdTag>;
using OperatorId = StrongId<OperatorIdTag>;
using WorkerId = StrongId<WorkerIdTag>;
using HealthSourceId = StrongId<HealthSourceIdTag>;
using GateId = StrongId<GateIdTag>;
using SnapshotId = StrongId<SnapshotIdTag>;
using JournalId = StrongId<JournalIdTag>;
using FailureDomainId = StrongId<FailureDomainIdTag>;
using VerifierId = StrongId<VerifierIdTag>;

// Monotonic counters that fence superseded authority. Explicit types so that a
// revision can never be passed where an epoch is expected.
enum class Revision : std::uint64_t {};
enum class Epoch : std::uint64_t {};
enum class AttemptEpoch : std::uint64_t {};
enum class Sequence : std::uint64_t {};
enum class GenerationCounter : std::uint64_t {};
enum class EpochCounter : std::uint64_t {};

[[nodiscard]] constexpr std::uint64_t value_of(Revision v) noexcept { return static_cast<std::uint64_t>(v); }
[[nodiscard]] constexpr std::uint64_t value_of(Epoch v) noexcept { return static_cast<std::uint64_t>(v); }
[[nodiscard]] constexpr std::uint64_t value_of(AttemptEpoch v) noexcept { return static_cast<std::uint64_t>(v); }
[[nodiscard]] constexpr std::uint64_t value_of(Sequence v) noexcept { return static_cast<std::uint64_t>(v); }
[[nodiscard]] constexpr std::uint64_t value_of(GenerationCounter v) noexcept {
  return static_cast<std::uint64_t>(v);
}
[[nodiscard]] constexpr std::uint64_t value_of(EpochCounter v) noexcept {
  return static_cast<std::uint64_t>(v);
}

[[nodiscard]] constexpr Revision next(Revision v) noexcept {
  return Revision{value_of(v) + 1};
}
[[nodiscard]] constexpr Sequence next(Sequence v) noexcept {
  return Sequence{value_of(v) + 1};
}

// ---------------------------------------------------------------------------
// Identifier derivation
// ---------------------------------------------------------------------------

// Domain separators hashed into derived identifiers. The values are part of the
// persisted identity space: changing one changes every identifier the runtime
// mints for that domain.
enum class IdDomain : std::uint16_t {
  kRollout = 1,
  kStage = 2,
  kCohort = 3,
  kAttempt = 4,
  kGeneration = 5,
  kPlan = 6,
  kAction = 7,
  kArtifact = 8,
  kEvidence = 9,
  kDecision = 10,
  kIncarnation = 11,
  kAuthority = 12,
  kOperator = 13,
  kWorker = 14,
  kHealthSource = 15,
  kGate = 16,
  kSnapshot = 17,
  kJournal = 18,
  kFailureDomain = 19,
  kVerifier = 20,
};

[[nodiscard]] std::string_view to_string(IdDomain domain) noexcept;

// Deterministic identifier factory. The same seed and the same call sequence
// produce the same identifiers, which is what makes seeded randomized
// state-machine tests reproducible. Production seeds come from OS entropy.
class IdFactory {
 public:
  IdFactory() noexcept;
  explicit IdFactory(const Digest256& seed) noexcept;

  // Fresh entropy from the operating system. Never fails silently: if the OS
  // entropy source is unavailable the process-mixing fallback is used.
  [[nodiscard]] static IdFactory from_entropy() noexcept;

  [[nodiscard]] const Digest256& seed() const noexcept { return seed_; }
  [[nodiscard]] std::uint64_t counter() const noexcept { return counter_; }

  template <class Id>
  [[nodiscard]] Id next(IdDomain domain) noexcept {
    const Digest256 value = derive(domain, counter_);
    ++counter_;
    return Id::from_bytes(std::span<const std::byte, Id::byte_size>(value.data(), Id::byte_size));
  }

  // Explicitly resumable form used by recovery, so that a restarted controller
  // never re-mints an identifier it already published.
  void set_counter(std::uint64_t counter) noexcept { counter_ = counter; }

 private:
  [[nodiscard]] Digest256 derive(IdDomain domain, std::uint64_t counter) const noexcept;

  Digest256 seed_{};
  std::uint64_t counter_ = 0;
};

// Hash support so identifiers can key unordered containers.
template <class Tag, std::size_t N = kDigest128Bytes>
struct StrongIdHash {
  [[nodiscard]] std::size_t operator()(const StrongId<Tag, N>& id) const noexcept {
    std::size_t h = 1469598103934665603ULL;
    for (const std::byte b : id.bytes()) {
      h ^= static_cast<std::size_t>(std::to_integer<unsigned char>(b));
      h *= 1099511628211ULL;
    }
    return h;
  }
};

}  // namespace rollout_fabric
