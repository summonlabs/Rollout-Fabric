// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/adapters.hpp"

#include <string_view>

namespace rollout_fabric {

IEvidenceSink::~IEvidenceSink() = default;
IExecutionAdapter::~IExecutionAdapter() = default;
IHealthAdapter::~IHealthAdapter() = default;

std::string_view to_string(DispatchOutcome outcome) noexcept {
  switch (outcome) {
    case DispatchOutcome::kAccepted: return "accepted";
    case DispatchOutcome::kRejected: return "rejected";
    case DispatchOutcome::kUnavailable: return "unavailable";
    case DispatchOutcome::kDuplicateSuppressed: return "duplicate_suppressed";
    case DispatchOutcome::kThrottled: return "throttled";
    case DispatchOutcome::kFenced: return "fenced";
  }
  return "unknown";
}

std::string_view to_string(ReconcileDisposition disposition) noexcept {
  switch (disposition) {
    case ReconcileDisposition::kCompleted: return "completed";
    case ReconcileDisposition::kFailed: return "failed";
    case ReconcileDisposition::kInProgress: return "in_progress";
    case ReconcileDisposition::kNotStarted: return "not_started";
    case ReconcileDisposition::kUnknown: return "unknown";
  }
  return "unknown";
}

}  // namespace rollout_fabric
