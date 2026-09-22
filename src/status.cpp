// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/status.hpp"

#include <string>

namespace rollout_fabric {

std::string_view to_string(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::kOk: return "ok";
    case StatusCode::kInvalidArgument: return "invalid_argument";
    case StatusCode::kNotFound: return "not_found";
    case StatusCode::kAlreadyExists: return "already_exists";
    case StatusCode::kConflict: return "conflict";
    case StatusCode::kPreconditionFailed: return "precondition_failed";
    case StatusCode::kPermissionDenied: return "permission_denied";
    case StatusCode::kResourceExhausted: return "resource_exhausted";
    case StatusCode::kUnavailable: return "unavailable";
    case StatusCode::kInternal: return "internal";
    case StatusCode::kCorrupt: return "corrupt";
    case StatusCode::kStaleEpoch: return "stale_epoch";
    case StatusCode::kStaleGeneration: return "stale_generation";
    case StatusCode::kStaleAttempt: return "stale_attempt";
    case StatusCode::kStaleEvidence: return "stale_evidence";
    case StatusCode::kStaleRevision: return "stale_revision";
    case StatusCode::kFenced: return "fenced";
    case StatusCode::kNotSupported: return "not_supported";
    case StatusCode::kCancelled: return "cancelled";
    case StatusCode::kShuttingDown: return "shutting_down";
    case StatusCode::kLimitExceeded: return "limit_exceeded";
    case StatusCode::kPolicyViolation: return "policy_violation";
    case StatusCode::kDeadlineExceeded: return "deadline_exceeded";
    case StatusCode::kIoError: return "io_error";
    case StatusCode::kLocked: return "locked";
    case StatusCode::kImmutable: return "immutable";
    case StatusCode::kUnsupportedGate: return "unsupported_gate";
  }
  return "unknown";
}

std::string Status::to_string() const {
  std::string text;
  text.reserve(message_.size() + 32);
  text.append(rollout_fabric::to_string(code_));
  if (!message_.empty()) {
    text.append(": ");
    text.append(message_);
  }
  return text;
}

Status make_status(StatusCode code, std::string message) {
  return Status(code, std::move(message));
}

}  // namespace rollout_fabric
