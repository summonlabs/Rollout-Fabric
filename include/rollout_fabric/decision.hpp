// Rollout Fabric - decisions and their explanations.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every state transition this runtime performs is the consequence of exactly
// one recorded decision. A decision names the inputs it saw (by digest), the
// policy that governed it, the evidence it weighed, the action it selected, the
// alternatives it rejected and why, and the generation, revision and controller
// incarnation under which it had the authority to act. Replaying the same
// inputs against the same policy produces the same decision and the same text.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rollout_fabric/codec.hpp"
#include "rollout_fabric/digest.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"

namespace rollout_fabric {

enum class DecisionKind : std::uint8_t {
  kValidatePlan = 0,
  kArmRollout = 1,
  kStartStage = 2,
  kDispatchBatch = 3,
  kHoldStage = 4,
  kOpenGate = 5,
  kRefuseGate = 6,
  kAdvanceStage = 7,
  kCompleteRollout = 8,
  kPauseRollout = 9,
  kResumeRollout = 10,
  kAbortRollout = 11,
  kBeginRollback = 12,
  kFailStage = 13,
  kFailRollout = 14,
  kRetireRollout = 15,
  kRetryTarget = 16,
  kCancelAttempt = 17,
  kReconcile = 18,
  kRejectEvidence = 19,
  kAdmitAttempt = 20,
};

[[nodiscard]] std::string_view to_string(DecisionKind kind) noexcept;

struct RejectedAlternative {
  std::string action;
  std::string reason;
};

struct Decision {
  DecisionId id{};
  DecisionKind kind = DecisionKind::kHoldStage;
  RolloutId rollout{};
  GenerationId generation{};
  Revision revision{};
  EpochCounter controller_epoch{};
  IncarnationId incarnation{};
  AuthorityId authority{};
  StageId stage{};
  TargetId target{};
  AttemptId attempt{};
  Timestamp decided_at{};
  Digest256 inputs_digest{};
  Digest256 policy_digest{};
  Digest256 evidence_digest{};
  std::string selected;
  std::vector<RejectedAlternative> rejected;
  std::string rationale;

  [[nodiscard]] Digest256 decision_digest() const;
  [[nodiscard]] std::string render() const;
};

void encode(ByteWriter& writer, const Decision& value);
[[nodiscard]] Result<Decision> decode_decision(ByteReader& reader, const RuntimeLimits& limits);

// Accumulates a decision's inputs so the digests and the text are built from
// the same material and cannot drift apart.
class DecisionBuilder {
 public:
  DecisionBuilder(DecisionKind kind, const RolloutId& rollout, const GenerationId& generation,
                  Revision revision, EpochCounter controller_epoch, const IncarnationId& incarnation);

  void stage(const StageId& stage) noexcept { decision_.stage = stage; }
  void target(const TargetId& target) noexcept { decision_.target = target; }
  void attempt(const AttemptId& attempt) noexcept { decision_.attempt = attempt; }
  void authority(const AuthorityId& authority) noexcept { decision_.authority = authority; }

  // Feeds one labelled input into the input digest.
  void input(std::string_view name, std::string_view value);
  void input_digest(std::string_view name, const Digest256& digest);
  void input_u64(std::string_view name, std::uint64_t value);

  void policy_digest(const Digest256& digest) noexcept { decision_.policy_digest = digest; }
  void evidence_digest(const Digest256& digest) noexcept { decision_.evidence_digest = digest; }

  void select(std::string selected);
  void reject(std::string action, std::string reason);
  void rationale(std::string text);

  [[nodiscard]] Decision finish(IdFactory& ids, Timestamp decided_at, const RuntimeLimits& limits);

 private:
  void append_input(std::string_view name, std::string_view value);

  Decision decision_{};
  ByteWriter input_bytes_;
};

}  // namespace rollout_fabric
