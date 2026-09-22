// Rollout Fabric - evidence vocabulary.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The evidence vocabulary lives in its own header because plans, policies and
// stage specifications all have to name an attesting authority without pulling
// in the record type. Nothing here knows about storage, gating or transport.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace rollout_fabric {

enum class EvidenceKind : std::uint8_t {
  // The worker accepted the attempt. Proves acceptance only.
  kDispatchAccepted = 0,
  // The worker is still working. Proves liveness only.
  kExecutionProgress = 1,
  // The effect completed. This is the only kind that completes an attempt.
  kExecutionCompleted = 2,
  // The effect failed. Terminal for the attempt.
  kExecutionFailed = 3,
  // A health observation about a target or its population.
  kHealthSample = 4,
  // Compensation finished, successfully or not.
  kRollbackCompleted = 5,
  kRollbackFailed = 6,
  // Independent verification of the post-change state.
  kVerificationPassed = 7,
  kVerificationFailed = 8,
  // A worker confirmed it stopped work on an attempt after a cancel request.
  kCancellationAcknowledged = 9,
};

// Broad classes. Gates are expressed in terms of classes, never individual
// kinds, so a new kind cannot accidentally satisfy an unrelated gate.
enum class EvidenceClass : std::uint8_t {
  kAcceptance = 0,
  kExecution = 1,
  kTelemetry = 2,
  kCompensation = 3,
  kVerification = 4,
  kCancellation = 5,
};

[[nodiscard]] EvidenceClass evidence_class(EvidenceKind kind) noexcept;
[[nodiscard]] std::string_view to_string(EvidenceKind kind) noexcept;
[[nodiscard]] std::optional<EvidenceKind> parse_evidence_kind(std::string_view text) noexcept;
[[nodiscard]] std::string_view to_string(EvidenceClass klass) noexcept;

enum class EvidenceOutcome : std::uint8_t {
  kSucceeded = 0,
  kFailed = 1,
  kInconclusive = 2,
};

[[nodiscard]] std::string_view to_string(EvidenceOutcome outcome) noexcept;

// Who is making the claim. A gate names the authority it requires; anything
// else is recorded and ignored.
enum class EvidenceAuthority : std::uint8_t {
  kWorkerSelfReported = 0,
  kExecutionAdapterAttested = 1,
  kVerifierAttested = 2,
  kHealthSourceAttested = 3,
  kOperatorAttested = 4,
};

[[nodiscard]] std::string_view to_string(EvidenceAuthority authority) noexcept;
[[nodiscard]] std::optional<EvidenceAuthority> parse_evidence_authority(std::string_view text) noexcept;

// Where the record came from. Anything restored from durable state is marked
// and can never satisfy a gate that requires a live observation.
enum class EvidenceOrigin : std::uint8_t {
  kLive = 0,
  kRecoveredFromJournal = 1,
  // A live re-query of the execution adapter after a restart. This is the only
  // way a pre-restart attempt is reconciled.
  kReconciledFromAdapter = 2,
};

[[nodiscard]] std::string_view to_string(EvidenceOrigin origin) noexcept;

}  // namespace rollout_fabric
