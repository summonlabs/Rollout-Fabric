// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The target worker process.
//
// A worker is a real operating system process with real threads and one real
// socket. It connects to the controller, announces itself with a Hello, and
// then serves dispatch requests until the controller shuts it down, the socket
// fails, or the failure-injection budget is spent.
//
// The threading contract is the one stated in worker.hpp and it is enforced by
// construction here:
//
//   * The reactor thread (the thread that called run()) owns the socket, the
//     effect table and every identifier the worker mints.
//   * One effect runs on one std::jthread. An effect thread performs a bounded
//     deterministic computation scaled by the scenario's work duration - it is
//     genuine CPU work, never a sleep pretending to be one - and then pushes a
//     completion record onto the completion queue.
//   * Exactly one mutex exists in the process and it guards exactly that queue.
//     It is never held across a socket call, a callback or an allocation that
//     can fail; the reactor drains the queue between socket operations.
//
// Dispatch is a request/ack protocol and completion is separate, later
// evidence. A worker that never completes an effect emits no terminal record at
// all, which is exactly what the controller must be able to survive.
#include "rollout_fabric/worker.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "rollout_fabric/digest.hpp"
#include "rollout_fabric/json.hpp"
#include "rollout_fabric/protocol.hpp"
#include "rollout_fabric/version.hpp"

namespace rollout_fabric {
namespace {

using SteadyClock = std::chrono::steady_clock;

// A scenario document is an operator supplied file. It is bounded before it is
// read, before it is parsed, and again when every field is converted.
constexpr std::uint64_t kMaxScenarioBytes = 1u << 20;
constexpr std::int64_t kMaxScenarioMillis = 3600000;  // one hour

// A work duration at or above this is long enough that a progress sample says
// something an operator can act on.
constexpr std::int64_t kProgressThresholdMillis = 20;

// A scenario that says "stale" without naming an age is reporting telemetry
// that is already older than the default evidence lifetime.
constexpr std::int64_t kStaleHealthDefaultSeconds = 60;

// Exit code of the deliberate crash-on-target injection.
constexpr int kCrashExitCode = 70;

// One iteration of the effect loop is this many mixing rounds, so the loop
// stays responsive to cancellation without becoming a sleep.
constexpr std::uint64_t kMixingRoundsPerSlice = 8192;
constexpr std::uint64_t kYieldEverySlices = 64;

// How long a deliberately crashing worker waits for its acknowledgement to
// leave the socket before it dies.
constexpr std::int64_t kCrashFlushMillis = 200;

// The longest a single select() may block in the reactor loop.
constexpr std::int64_t kReactorSliceMillis = 5;

[[nodiscard]] Timestamp wall_now() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return Timestamp::from_unix_nanos(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// Target identity derivation, byte for byte the rule src/plan_json.cpp uses, so
// that a controller which derived the target from the plan document and a
// worker which reads a scenario by name agree on one identifier.
[[nodiscard]] TargetId derive_target_id(std::string_view name) noexcept {
  Sha256 hasher;
  hasher.update(std::string_view("rollout-fabric/target/v1"));
  hasher.update(name);
  const Digest256 digest = hasher.finish();
  return TargetId::from_bytes(
      std::span<const std::byte, TargetId::byte_size>(digest.data(), TargetId::byte_size));
}

[[nodiscard]] std::string join_path(std::string_view parent, std::string_view key) {
  if (parent.empty()) {
    return std::string(key);
  }
  std::string out(parent);
  out.push_back('.');
  out.append(key);
  return out;
}

[[nodiscard]] std::string index_path(std::string_view parent, std::size_t index) {
  std::string out(parent);
  out.push_back('[');
  out.append(std::to_string(index));
  out.push_back(']');
  return out;
}

// --- file reading -----------------------------------------------------------

#if defined(_WIN32)
[[nodiscard]] Result<std::string> read_text_file(const std::string& path, std::uint64_t max_bytes) {
  if (path.empty()) {
    return make_status(StatusCode::kInvalidArgument, "the scenario path is empty");
  }
  const int needed = ::MultiByteToWideChar(CP_UTF8, 0, path.data(),
                                           static_cast<int>(path.size()), nullptr, 0);
  if (needed <= 0) {
    return make_status(StatusCode::kInvalidArgument, "the scenario path is not valid UTF-8");
  }
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wide.data(),
                            needed) != needed) {
    return make_status(StatusCode::kInvalidArgument, "the scenario path is not valid UTF-8");
  }
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return make_status(StatusCode::kNotFound, "the scenario file does not exist");
    }
    return make_status(StatusCode::kIoError,
                       "cannot open the scenario file: Windows error " + std::to_string(error));
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(handle, &size) == 0) {
    (void)::CloseHandle(handle);
    return make_status(StatusCode::kIoError, "cannot determine the scenario file size");
  }
  if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
    (void)::CloseHandle(handle);
    return make_status(StatusCode::kLimitExceeded,
                       "the scenario file is larger than the configured ceiling of " +
                           std::to_string(max_bytes) + " byte(s)");
  }
  std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
  std::size_t total = 0;
  while (total < text.size()) {
    const DWORD want = static_cast<DWORD>(
        std::min<std::size_t>(text.size() - total, static_cast<std::size_t>(1u << 20)));
    DWORD got = 0;
    if (::ReadFile(handle, text.data() + total, want, &got, nullptr) == 0) {
      (void)::CloseHandle(handle);
      return make_status(StatusCode::kIoError,
                         "cannot read the scenario file: Windows error " +
                             std::to_string(::GetLastError()));
    }
    if (got == 0) {
      break;
    }
    total += got;
  }
  (void)::CloseHandle(handle);
  text.resize(total);
  return text;
}
#else
[[nodiscard]] Result<std::string> read_text_file(const std::string& path, std::uint64_t max_bytes) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return make_status(StatusCode::kNotFound, "the scenario file does not exist");
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > max_bytes) {
    return make_status(StatusCode::kLimitExceeded, "the scenario file is larger than the ceiling");
  }
  stream.seekg(0, std::ios::beg);
  std::string text;
  text.resize(static_cast<std::size_t>(size));
  stream.read(text.data(), size);
  return text;
}
#endif

// --- JSON helpers -----------------------------------------------------------

[[nodiscard]] Status read_optional_bool(const JsonValue& object, std::string_view key,
                                        std::string_view path, bool& out) {
  const JsonValue* member = object.find(key);
  if (member == nullptr) {
    return Status::success();
  }
  auto value = member->require_bool(join_path(path, key));
  if (!value.ok()) {
    return value.status();
  }
  out = value.value();
  return Status::success();
}

// Durations in a scenario document are integer milliseconds. Negative values and
// values beyond the ceiling are refused rather than clamped, so a typo in a
// failure-injection document cannot silently become a different experiment.
[[nodiscard]] Result<Duration> read_optional_millis(const JsonValue& object, std::string_view key,
                                                    std::string_view path, Duration fallback) {
  const JsonValue* member = object.find(key);
  if (member == nullptr) {
    return fallback;
  }
  const std::string field = join_path(path, key);
  auto value = member->require_integer(field);
  if (!value.ok()) {
    return value.status();
  }
  if (value.value() < 0 || value.value() > kMaxScenarioMillis) {
    return make_status(StatusCode::kInvalidArgument,
                       field + " must be between 0 and " + std::to_string(kMaxScenarioMillis) +
                           " milliseconds");
  }
  return Duration::from_millis(value.value());
}

// --- scenario parsing -------------------------------------------------------

[[nodiscard]] Status parse_target_scenario(const JsonValue& value, std::string_view path,
                                           const RuntimeLimits& limits,
                                           WorkerTargetScenario& out) {
  auto object = value.require_object(path);
  if (!object.ok()) {
    return object.status();
  }
  const JsonValue& members = *object.value();
  out = WorkerTargetScenario{};
  const JsonValue* name = members.find("name");
  if (name == nullptr) {
    // An unnamed entry is the declaration that matches every target.
    out.matches_all = true;
  } else {
    const std::string name_path = join_path(path, "name");
    auto text = name->require_string(name_path);
    if (!text.ok()) {
      return text.status();
    }
    if (text.value().empty() || text.value() == "*") {
      out.matches_all = true;
    } else {
      if (text.value().size() > limits.max_name_bytes) {
        return make_status(StatusCode::kLimitExceeded,
                           name_path + " is longer than max_name_bytes");
      }
      out.target = derive_target_id(text.value());
      out.matches_all = false;
    }
  }

  const auto duration = [&](std::string_view key, Duration fallback) -> Result<Duration> {
    return read_optional_millis(members, key, path, fallback);
  };

  auto work = duration("work_duration_ms", out.work_duration);
  if (!work.ok()) {
    return work.status();
  }
  out.work_duration = work.value();
  auto age = duration("health_age_ms", out.health_age);
  if (!age.ok()) {
    return age.status();
  }
  out.health_age = age.value();

  const auto flag = [&](std::string_view key, bool& target) -> Status {
    return read_optional_bool(members, key, path, target);
  };
  if (const Status status = flag("fail", out.fail); !status.ok()) return status;
  if (const Status status = flag("rollback_fails", out.rollback_fails); !status.ok()) return status;
  if (const Status status = flag("never_complete", out.never_complete); !status.ok()) return status;
  if (const Status status = flag("duplicate_report", out.duplicate_report); !status.ok()) return status;
  if (const Status status = flag("reorder_report", out.reorder_report); !status.ok()) return status;
  if (const Status status = flag("stale_health", out.stale_health); !status.ok()) return status;
  if (const Status status = flag("always_unhealthy", out.always_unhealthy); !status.ok()) return status;
  if (const Status status = flag("crash_on_target", out.crash_on_target); !status.ok()) return status;
  return Status::success();
}

}  // namespace

// ---------------------------------------------------------------------------
// WorkerScenario
// ---------------------------------------------------------------------------

Result<WorkerScenario> WorkerScenario::parse(std::string_view json_text, const RuntimeLimits& limits) {
  const Status limits_status = limits.validate();
  if (!limits_status.ok()) {
    return limits_status;
  }
  if (json_text.empty()) {
    return make_status(StatusCode::kInvalidArgument, "the scenario document is empty");
  }
  if (json_text.size() > kMaxScenarioBytes) {
    return make_status(StatusCode::kLimitExceeded,
                       "the scenario document is " + std::to_string(json_text.size()) +
                           " byte(s), above the ceiling of " + std::to_string(kMaxScenarioBytes));
  }
  auto document = parse_json(json_text, limits);
  if (!document.ok()) {
    return document.status();
  }
  auto root = document.value().require_object("scenario");
  if (!root.ok()) {
    return root.status();
  }
  const JsonValue& object = *root.value();

  WorkerScenario scenario;
  if (const JsonValue* fallback = object.find("fallback"); fallback != nullptr) {
    const Status status = parse_target_scenario(*fallback, "fallback", limits, scenario.fallback);
    if (!status.ok()) {
      return status;
    }
    // Whatever the fallback entry is called, it is the entry that answers for
    // every target the document does not name.
    scenario.fallback.matches_all = true;
  }

  if (const JsonValue* targets = object.find("targets"); targets != nullptr) {
    auto array = targets->require_array("targets");
    if (!array.ok()) {
      return array.status();
    }
    const std::vector<JsonValue>& items = array.value()->items();
    if (items.size() > limits.max_total_targets) {
      return make_status(StatusCode::kLimitExceeded,
                         "the scenario declares " + std::to_string(items.size()) +
                             " targets, above max_total_targets");
    }
    scenario.targets.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
      WorkerTargetScenario entry;
      const std::string path = index_path("targets", index);
      const Status status = parse_target_scenario(items[index], path, limits, entry);
      if (!status.ok()) {
        return status;
      }
      if (entry.matches_all) {
        return make_status(StatusCode::kInvalidArgument,
                           path + " declares no name; only the fallback entry may match every "
                                  "target");
      }
      scenario.targets.push_back(std::move(entry));
    }
  }

  // Canonical order by target identity: lookup is a binary search and two
  // documents that declare the same targets in different orders behave
  // identically.
  std::sort(scenario.targets.begin(), scenario.targets.end(),
            [](const WorkerTargetScenario& left, const WorkerTargetScenario& right) {
              return left.target < right.target;
            });
  for (std::size_t index = 1; index < scenario.targets.size(); ++index) {
    if (scenario.targets[index - 1].target == scenario.targets[index].target) {
      return make_status(StatusCode::kAlreadyExists,
                         "targets[" + std::to_string(index) + "] repeats a target that is already "
                         "declared earlier in the document");
    }
  }
  return scenario;
}

Result<WorkerScenario> WorkerScenario::load(const std::string& path, const RuntimeLimits& limits) {
  auto text = read_text_file(path, kMaxScenarioBytes);
  if (!text.ok()) {
    return text.status();
  }
  return parse(text.value(), limits);
}

const WorkerTargetScenario& WorkerScenario::for_target(const TargetId& target) const noexcept {
  if (!target.is_nil()) {
    const auto found = std::lower_bound(
        targets.begin(), targets.end(), target,
        [](const WorkerTargetScenario& entry, const TargetId& probe) { return entry.target < probe; });
    if (found != targets.end() && found->target == target) {
      return *found;
    }
  }
  return fallback;
}

// ---------------------------------------------------------------------------
// Effect execution
// ---------------------------------------------------------------------------
namespace {

enum class CompletionKind : std::uint8_t {
  kProgress = 0,
  kTerminal = 1,
  // The effect stopped because it was cancelled. It publishes no outcome.
  kStopped = 2,
};

// What an effect thread hands back to the reactor. The record itself is built
// by the reactor, which owns the identifiers and the socket.
struct Completion {
  AttemptId attempt{};
  CompletionKind kind = CompletionKind::kTerminal;
  EvidenceKind evidence_kind = EvidenceKind::kExecutionCompleted;
  EvidenceOutcome outcome = EvidenceOutcome::kSucceeded;
  Timestamp observed_at{};
};

// The one mutex in the worker, guarding the one queue it is allowed to guard.
// push() moves a small value into a vector; it never allocates a large buffer,
// never touches a socket and never calls back into anything.
struct CompletionQueue {
  std::mutex mutex;
  std::vector<Completion> items;

  void push(Completion completion) {
    std::lock_guard<std::mutex> guard(mutex);
    items.push_back(std::move(completion));
  }

  [[nodiscard]] std::vector<Completion> drain() {
    std::vector<Completion> batch;
    std::lock_guard<std::mutex> guard(mutex);
    batch.swap(items);
    return batch;
  }
};

// Reactor-owned record of one served attempt. It is the idempotency table, the
// reconcile answer and the evidence source of truth, and it is only ever
// touched by the reactor thread.
struct EffectState {
  AttemptFence fence{};
  Digest256 key{};
  TargetId target{};
  std::string target_name;
  CohortId cohort{};
  WorkerTargetScenario scenario{};
  std::shared_ptr<std::atomic<bool>> stop{};
  Timestamp accepted_at{};
  Timestamp terminal_at{};
  Sequence sequence{};
  bool running = false;
  bool terminal = false;
  bool cancelled = false;
  bool duplicate_report = false;
  // Set when a repeated dispatch arrived while the effect was still running:
  // that fence is answered with the recorded outcome as soon as there is one.
  bool has_deferred = false;
  AttemptFence deferred{};
  EvidenceKind kind = EvidenceKind::kExecutionCompleted;
  EvidenceOutcome outcome = EvidenceOutcome::kInconclusive;
};

struct WorkPlan {
  AttemptId attempt{};
  Duration work_duration{};
  bool fail = false;
  bool never_complete = false;
  bool emit_progress = false;
  std::uint64_t seed = 0;
};

// The effect itself: a deterministic mixing loop scaled by the scenario's work
// duration. It burns real CPU for as long as the scenario asks and checks the
// cancellation flag between slices, so a cancelled effect stops promptly and
// never publishes an outcome.
void run_effect(const std::shared_ptr<std::atomic<bool>>& stop, CompletionQueue& queue,
                const WorkPlan& plan) {
  const auto started = SteadyClock::now();
  const auto budget = std::chrono::nanoseconds(plan.work_duration.nanos() > 0
                                                   ? plan.work_duration.nanos()
                                                   : 0);
  const auto half = budget / 2;
  bool progress_sent = !plan.emit_progress;
  std::uint64_t mix = plan.seed | 1u;
  std::uint64_t slices = 0;

  for (;;) {
    for (std::uint64_t round = 0; round < kMixingRoundsPerSlice; ++round) {
      mix ^= mix << 13;
      mix ^= mix >> 7;
      mix ^= mix << 17;
      mix += 0x9E3779B97F4A7C15ull;
    }
    ++slices;
    if (stop->load(std::memory_order_relaxed)) {
      Completion stopped;
      stopped.attempt = plan.attempt;
      stopped.kind = CompletionKind::kStopped;
      stopped.observed_at = wall_now();
      queue.push(std::move(stopped));
      return;
    }
    const auto elapsed = SteadyClock::now() - started;
    if (!progress_sent && elapsed >= half) {
      progress_sent = true;
      Completion progress;
      progress.attempt = plan.attempt;
      progress.kind = CompletionKind::kProgress;
      progress.evidence_kind = EvidenceKind::kExecutionProgress;
      progress.outcome = EvidenceOutcome::kInconclusive;
      progress.observed_at = wall_now();
      queue.push(std::move(progress));
    }
    if (elapsed >= budget) {
      break;
    }
    if (slices % kYieldEverySlices == 0) {
      std::this_thread::yield();
    }
  }

  if (plan.never_complete) {
    // The worker accepted the attempt and will never report a terminal outcome.
    // Dispatch is not completion; this is that case in its purest form.
    return;
  }
  Completion terminal;
  terminal.attempt = plan.attempt;
  terminal.kind = CompletionKind::kTerminal;
  terminal.evidence_kind =
      plan.fail ? EvidenceKind::kExecutionFailed : EvidenceKind::kExecutionCompleted;
  terminal.outcome = plan.fail ? EvidenceOutcome::kFailed : EvidenceOutcome::kSucceeded;
  terminal.observed_at = wall_now();
  queue.push(std::move(terminal));
}

[[nodiscard]] std::uint64_t mix_seed(const AttemptId& attempt) noexcept {
  std::uint64_t seed = 1469598103934665603ull;
  for (const std::byte value : attempt.bytes()) {
    seed ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(value));
    seed *= 1099511628211ull;
  }
  return seed;
}

// A message is encoded, bounded and only then queued, so a scenario that
// produced an oversized record would be refused before it reached the socket.
[[nodiscard]] Status queue_message(FrameChannel& channel, MessageType type, Sequence sequence,
                                   std::vector<std::byte> body, const RuntimeLimits& limits) {
  if (body.size() > limits.max_frame_bytes) {
    return make_status(StatusCode::kLimitExceeded,
                       "the encoded message is larger than max_frame_bytes");
  }
  return channel.queue(make_frame(type, sequence, std::move(body)));
}

}  // namespace

// ---------------------------------------------------------------------------
// Implementation state
// ---------------------------------------------------------------------------

struct Worker::Impl {
  explicit Impl(WorkerOptions value) : options(std::move(value)) {}

  WorkerOptions options{};
  // Reactor owned. Nothing below is touched by an effect thread.
  IdFactory ids{};
  std::map<AttemptId, EffectState> effects{};
  std::deque<AttemptId> effect_order{};
  std::map<Digest256, AttemptId> key_index{};
  std::vector<std::jthread> threads{};
  CompletionQueue completions{};
  FrameChannel channel{};
  std::uint64_t out_sequence = 0;
  std::uint64_t served_attempts = 0;
  std::uint64_t completed_attempts = 0;
  std::uint64_t failed_attempts = 0;
  std::uint32_t capacity = 1;
  IncarnationId controller_incarnation{};
  EpochCounter controller_epoch{0};
  bool accept_work = true;
  bool shutting_down = false;

  [[nodiscard]] Sequence next_sequence() noexcept { return Sequence{++out_sequence}; }

  [[nodiscard]] std::uint32_t running_effects() const noexcept {
    std::uint32_t count = 0;
    for (const auto& entry : effects) {
      if (entry.second.running) {
        ++count;
      }
    }
    return count;
  }

  // Bounds the effect table. Only a finished effect may be evicted and the
  // victim is chosen deterministically from the admission order, never from map
  // iteration order.
  void evict_if_needed() {
    const std::size_t room = options.limits.max_idempotency_entries;
    if (effects.size() < room) {
      return;
    }
    const std::size_t order = effect_order.size();
    for (std::size_t step = 0; step < order; ++step) {
      const AttemptId candidate = effect_order.front();
      effect_order.pop_front();
      const auto found = effects.find(candidate);
      if (found == effects.end()) {
        continue;
      }
      if (found->second.running) {
        effect_order.push_back(candidate);
        continue;
      }
      key_index.erase(found->second.key);
      effects.erase(found);
      return;
    }
  }

  [[nodiscard]] EffectState* find_effect(const AttemptId& attempt) noexcept {
    const auto found = effects.find(attempt);
    return found == effects.end() ? nullptr : &found->second;
  }

  [[nodiscard]] Status emit_record(const EffectState& effect, const AttemptFence& fence,
                                   EvidenceKind kind, EvidenceOutcome outcome, Sequence sequence,
                                   Timestamp observed_at) {
    EvidenceRecord record;
    record.id = ids.next<EvidenceId>(IdDomain::kEvidence);
    record.rollout = fence.rollout;
    record.generation = fence.generation;
    record.stage = fence.stage;
    record.cohort = effect.cohort;
    record.target = effect.target;
    record.attempt = fence.attempt;
    record.attempt_epoch = fence.epoch;
    record.incarnation = fence.incarnation;
    record.kind = kind;
    record.outcome = outcome;
    record.authority = EvidenceAuthority::kWorkerSelfReported;
    record.origin = EvidenceOrigin::kLive;
    record.sequence = sequence;
    record.observed_at = observed_at;
    record.recorded_at = wall_now();
    record.payload_digest = sha256(std::string(to_string(kind)));
    const EvidenceMessage message{record};
    return queue_message(channel, MessageType::kEvidence, next_sequence(),
                         encode(message, options.limits), options.limits);
  }

  [[nodiscard]] Status send_ack(const DispatchAckMessage& ack) {
    return queue_message(channel, MessageType::kDispatchAck, next_sequence(), encode(ack),
                         options.limits);
  }

  // Records the terminal outcome in the effect table so that a repeated
  // dispatch of the same idempotency key replays it instead of performing the
  // effect twice.
  Status publish_terminal(EffectState& effect, const AttemptFence& fence, EvidenceKind kind,
                          EvidenceOutcome outcome, Timestamp observed_at) {
    effect.sequence = Sequence{value_of(effect.sequence) + 1};
    const Status status = emit_record(effect, fence, kind, outcome, effect.sequence, observed_at);
    effect.terminal = true;
    effect.running = false;
    effect.kind = kind;
    effect.outcome = outcome;
    if (outcome == EvidenceOutcome::kSucceeded) {
      ++completed_attempts;
    } else {
      ++failed_attempts;
    }
    return status;
  }

  [[nodiscard]] Status handle_dispatch(const Frame& frame);
  [[nodiscard]] Status handle_cancel(const Frame& frame);
  [[nodiscard]] Status handle_reconcile(const Frame& frame);
  [[nodiscard]] Status handle_health(const Frame& frame);
  [[nodiscard]] Status handle_heartbeat_ack(const Frame& frame);
  [[nodiscard]] Status drain_completions();
};

// --- dispatch ---------------------------------------------------------------

Status Worker::Impl::handle_dispatch(const Frame& frame) {
  auto decoded = decode_dispatch(frame.body, options.limits);
  if (!decoded.ok()) {
    return decoded.status();
  }
  const DispatchCommandMessage& message = decoded.value();
  const DispatchRequest& request = message.request;
  const Timestamp now = wall_now();

  // The target is what the effect changes. A dispatch that names no target
  // cannot be attributed to a scenario or reported against, so it is refused
  // rather than guessed at.
  if (message.target.is_nil()) {
    DispatchAckMessage ack;
    ack.fence = request.fence;
    ack.outcome = DispatchOutcome::kFenced;
    ack.detail = "the dispatch names no target";
    ack.accepted_at = now;
    return send_ack(ack);
  }

  if (request.fence.attempt.is_nil()) {
    DispatchAckMessage ack;
    ack.fence = request.fence;
    ack.outcome = DispatchOutcome::kFenced;
    ack.detail = "the dispatch names no attempt";
    ack.accepted_at = now;
    return send_ack(ack);
  }

  // Idempotency first: a key this worker has already seen must never start a
  // second effect, whatever else the dispatch says.
  const auto known = key_index.find(request.idempotency_key);
  if (known != key_index.end()) {
    EffectState* effect = find_effect(known->second);
    if (effect == nullptr) {
      key_index.erase(known);
    } else {
      DispatchAckMessage ack;
      ack.fence = request.fence;
      ack.outcome = DispatchOutcome::kDuplicateSuppressed;
      ack.detail = "this worker already holds the idempotency key; the effect was not started "
                   "again and its recorded outcome is what follows";
      ack.accepted_at = now;
      if (effect->terminal) {
        // Replay the recorded outcome under the fence that just asked for it.
        const Status status = emit_record(*effect, request.fence, effect->kind, effect->outcome,
                                          effect->sequence, effect->terminal_at);
        if (!status.ok()) {
          return status;
        }
        if (effect->duplicate_report) {
          const Status again = emit_record(*effect, request.fence, effect->kind, effect->outcome,
                                           effect->sequence, effect->terminal_at);
          if (!again.ok()) {
            return again;
          }
        }
      } else {
        // The effect is still running: remember the asking fence so the recorded
        // outcome is published for it too, once there is one.
        effect->has_deferred = true;
        effect->deferred = request.fence;
      }
      ++served_attempts;
      return send_ack(ack);
    }
  }

  if (!accept_work) {
    DispatchAckMessage ack;
    ack.fence = request.fence;
    ack.outcome = DispatchOutcome::kUnavailable;
    ack.detail = "the controller told this worker to stop accepting work";
    ack.accepted_at = now;
    return send_ack(ack);
  }

  if (running_effects() >= capacity) {
    DispatchAckMessage ack;
    ack.fence = request.fence;
    ack.outcome = DispatchOutcome::kThrottled;
    ack.detail = "the worker is already running its capacity of attempts";
    ack.accepted_at = now;
    return send_ack(ack);
  }

  const WorkerTargetScenario& scenario = options.scenario.for_target(message.target);
  evict_if_needed();

  EffectState effect;
  effect.fence = request.fence;
  effect.key = request.idempotency_key;
  effect.target = message.target;
  effect.target_name = message.target_name;
  effect.cohort = request.cohort;
  effect.scenario = scenario;
  effect.stop = std::make_shared<std::atomic<bool>>(false);
  effect.accepted_at = now;
  effect.sequence = Sequence{1};
  effect.running = !scenario.never_complete;
  effect.duplicate_report = scenario.duplicate_report;

  effects.emplace(request.fence.attempt, effect);
  key_index.emplace(request.idempotency_key, request.fence.attempt);
  effect_order.push_back(request.fence.attempt);
  ++served_attempts;

  // Acceptance is reported immediately and proves only that the worker took the
  // work. Completion is a separate, later record.
  const EffectState& stored = *find_effect(request.fence.attempt);
  const Status accepted = emit_record(stored, request.fence, EvidenceKind::kDispatchAccepted,
                                      EvidenceOutcome::kSucceeded, Sequence{1}, now);
  if (!accepted.ok()) {
    return accepted;
  }

  DispatchAckMessage ack;
  ack.fence = request.fence;
  ack.outcome = DispatchOutcome::kAccepted;
  ack.detail = "the worker accepted the attempt and will report completion separately";
  ack.accepted_at = now;
  const Status sent = send_ack(ack);
  if (!sent.ok()) {
    return sent;
  }

  if (scenario.crash_on_target) {
    // The acknowledgement is pushed out first so the controller observes a
    // worker that accepted work and then died, which is the failure the
    // reconcile path exists for.
    (void)channel.flush_out(Duration::from_millis(kCrashFlushMillis));
    std::_Exit(kCrashExitCode);
  }

  if (!scenario.never_complete) {
    WorkPlan plan;
    plan.attempt = request.fence.attempt;
    plan.work_duration = scenario.work_duration;
    plan.fail = scenario.fail;
    plan.never_complete = scenario.never_complete;
    plan.emit_progress = scenario.work_duration >= Duration::from_millis(kProgressThresholdMillis);
    plan.seed = mix_seed(request.fence.attempt);
    const auto stop = stored.stop;
    threads.emplace_back([stop, this, plan]() { run_effect(stop, completions, plan); });
  }
  return Status::success();
}

// --- completion drain -------------------------------------------------------

Status Worker::Impl::drain_completions() {
  std::vector<Completion> batch = completions.drain();
  for (const Completion& completion : batch) {
    EffectState* effect = find_effect(completion.attempt);
    if (effect == nullptr) {
      continue;
    }
    switch (completion.kind) {
      case CompletionKind::kStopped:
        effect->running = false;
        break;
      case CompletionKind::kProgress: {
        if (effect->terminal) {
          break;
        }
        const Status status =
            emit_record(*effect, effect->fence, EvidenceKind::kExecutionProgress,
                        EvidenceOutcome::kInconclusive,
                        Sequence{value_of(effect->sequence) + 1}, completion.observed_at);
        if (!status.ok()) {
          return status;
        }
        effect->sequence = Sequence{value_of(effect->sequence) + 1};
        break;
      }
      case CompletionKind::kTerminal: {
        if (effect->terminal || effect->cancelled) {
          // A cancelled attempt must never publish a success record, and a
          // second terminal outcome would be a second claim about one effect.
          effect->running = false;
          break;
        }
        const Status status = publish_terminal(*effect, effect->fence, completion.evidence_kind,
                                               completion.outcome, completion.observed_at);
        if (!status.ok()) {
          return status;
        }
        effect->terminal_at = completion.observed_at;
        if (effect->scenario.duplicate_report) {
          const Status again = emit_record(*effect, effect->fence, effect->kind, effect->outcome,
                                           effect->sequence, completion.observed_at);
          if (!again.ok()) {
            return again;
          }
        }
        if (effect->scenario.reorder_report) {
          // The terminal record has been published; a progress record carrying
          // the next sequence value follows it, so the controller sees an extra
          // record that describes an older observation.
          effect->sequence = Sequence{value_of(effect->sequence) + 1};
          const Status extra = emit_record(*effect, effect->fence, EvidenceKind::kExecutionProgress,
                                           EvidenceOutcome::kInconclusive, effect->sequence,
                                           completion.observed_at);
          if (!extra.ok()) {
            return extra;
          }
        }
        if (effect->has_deferred) {
          const AttemptFence deferred = effect->deferred;
          effect->has_deferred = false;
          const Status replay = emit_record(*effect, deferred, effect->kind, effect->outcome,
                                            effect->sequence, completion.observed_at);
          if (!replay.ok()) {
            return replay;
          }
          if (effect->duplicate_report) {
            const Status replay_again = emit_record(*effect, deferred, effect->kind,
                                                    effect->outcome, effect->sequence,
                                                    completion.observed_at);
            if (!replay_again.ok()) {
              return replay_again;
            }
          }
        }
        break;
      }
    }
  }
  return Status::success();
}

// --- cancel -----------------------------------------------------------------

Status Worker::Impl::handle_cancel(const Frame& frame) {
  auto decoded = decode_cancel(frame.body, options.limits);
  if (!decoded.ok()) {
    return decoded.status();
  }
  const CancelMessage& message = decoded.value();
  CancelAckMessage ack;
  ack.fence = message.fence;

  EffectState* effect = find_effect(message.fence.attempt);
  if (effect == nullptr) {
    ack.stopped = false;
    ack.detail = "this worker has no record of the attempt";
    return queue_message(channel, MessageType::kCancelAck, next_sequence(), encode(ack),
                         options.limits);
  }

  if (effect->running && effect->stop != nullptr) {
    effect->cancelled = true;
    effect->stop->store(true, std::memory_order_relaxed);
    ack.stopped = true;
    ack.detail = "the effect was asked to stop and will not publish an outcome";
  } else {
    ack.stopped = false;
    ack.detail = effect->terminal ? "the attempt already reached an outcome"
                                  : "the attempt had already stopped";
  }

  effect->sequence = Sequence{value_of(effect->sequence) + 1};
  const Status status = emit_record(*effect, effect->fence, EvidenceKind::kCancellationAcknowledged,
                                    EvidenceOutcome::kInconclusive, effect->sequence, wall_now());
  if (!status.ok()) {
    return status;
  }
  return queue_message(channel, MessageType::kCancelAck, next_sequence(), encode(ack),
                       options.limits);
}

// --- reconcile --------------------------------------------------------------

Status Worker::Impl::handle_reconcile(const Frame& frame) {
  auto decoded = decode_reconcile_request(frame.body, options.limits);
  if (!decoded.ok()) {
    return decoded.status();
  }
  const ReconcileRequestMessage& message = decoded.value();
  ReconcileResponseMessage response;
  response.request_id = message.request_id;
  response.entries.reserve(message.fences.size());
  for (const AttemptFence& fence : message.fences) {
    ReconcileEntry entry;
    entry.fence = fence;
    const EffectState* effect = find_effect(fence.attempt);
    if (effect == nullptr) {
      entry.disposition = ReconcileDisposition::kNotStarted;
      entry.outcome = EvidenceOutcome::kInconclusive;
      entry.detail = "this worker has no record of the attempt";
    } else if (effect->running) {
      entry.disposition = ReconcileDisposition::kInProgress;
      entry.outcome = EvidenceOutcome::kInconclusive;
      entry.detail = "the effect is still running";
    } else if (effect->terminal) {
      entry.disposition = effect->outcome == EvidenceOutcome::kSucceeded
                              ? ReconcileDisposition::kCompleted
                              : ReconcileDisposition::kFailed;
      entry.outcome = effect->outcome;
      entry.detail = "the recorded outcome of the effect";
    } else {
      entry.disposition = ReconcileDisposition::kFailed;
      entry.outcome = EvidenceOutcome::kInconclusive;
      entry.detail = "the effect was cancelled and will never report an outcome";
    }
    response.entries.push_back(std::move(entry));
  }
  return queue_message(channel, MessageType::kReconcileResponse, next_sequence(),
                       encode(response), options.limits);
}

// --- health -----------------------------------------------------------------

Status Worker::Impl::handle_health(const Frame& frame) {
  auto decoded = decode_health_request(frame.body, options.limits);
  if (!decoded.ok()) {
    return decoded.status();
  }
  const HealthRequestMessage& message = decoded.value();
  const Timestamp now = wall_now();
  HealthResponseMessage response;
  response.request_id = message.request_id;
  response.observed_at = now;
  response.live = true;
  response.samples.reserve(message.targets.size());
  for (const TargetId& target : message.targets) {
    const WorkerTargetScenario& scenario = options.scenario.for_target(target);
    Duration age = scenario.health_age;
    if (scenario.stale_health && age.is_zero()) {
      age = Duration::from_seconds(kStaleHealthDefaultSeconds);
    }
    HealthSample sample;
    sample.target = target;
    sample.healthy = !scenario.always_unhealthy && !scenario.fail;
    sample.observed_at = now - age;
    if (scenario.always_unhealthy) {
      sample.reason = "the scenario reports this target as permanently unhealthy";
    } else if (scenario.fail) {
      sample.reason = "the effect for this target is configured to fail";
    } else {
      sample.reason = "reported healthy by the worker's scenario";
    }
    response.samples.push_back(std::move(sample));
  }
  return queue_message(channel, MessageType::kHealthResponse, next_sequence(),
                       encode(response, options.limits), options.limits);
}

// --- heartbeat --------------------------------------------------------------

Status Worker::Impl::handle_heartbeat_ack(const Frame& frame) {
  auto decoded = decode_heartbeat_ack(frame.body, options.limits);
  if (!decoded.ok()) {
    return decoded.status();
  }
  controller_incarnation = decoded.value().controller_incarnation;
  controller_epoch = decoded.value().controller_epoch;
  accept_work = decoded.value().accept_work;
  return Status::success();
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

Worker::Worker() = default;
Worker::~Worker() = default;

Result<std::unique_ptr<Worker>> Worker::create(WorkerOptions options) {
  const Status limits_status = options.limits.validate();
  if (!limits_status.ok()) {
    return limits_status;
  }
  if (options.controller_port == 0) {
    return make_status(StatusCode::kInvalidArgument,
                       "a worker needs the controller's port; zero selects nothing");
  }
  if (options.capacity == 0) {
    return make_status(StatusCode::kInvalidArgument, "a worker must accept at least one attempt");
  }
  if (options.capacity > options.limits.max_concurrent_attempts) {
    return make_status(StatusCode::kLimitExceeded,
                       "the requested capacity exceeds max_concurrent_attempts");
  }
  if (options.heartbeat_interval.is_negative()) {
    return make_status(StatusCode::kInvalidArgument, "the heartbeat interval must not be negative");
  }
  if (options.name.size() > options.limits.max_name_bytes) {
    return make_status(StatusCode::kLimitExceeded, "the worker name is longer than max_name_bytes");
  }

  auto worker = std::unique_ptr<Worker>(new Worker());
  worker->impl_ = std::make_unique<Impl>(std::move(options));
  Impl& state = *worker->impl_;
  state.ids = IdFactory::from_entropy();
  worker->worker_id_ = state.ids.next<WorkerId>(IdDomain::kWorker);
  worker->incarnation_ = state.ids.next<IncarnationId>(IdDomain::kIncarnation);
  state.capacity = state.options.capacity;
  worker->ready_line_ = "rf-worker id=" + worker->worker_id_.to_hex() +
                        " incarnation=" + worker->incarnation_.to_hex() +
                        " controller_port=" + std::to_string(state.options.controller_port) +
                        " capacity=" + std::to_string(state.options.capacity);
  return worker;
}

Status Worker::run() {
  if (impl_ == nullptr) {
    return make_status(StatusCode::kInternal, "the worker was not created");
  }
  Impl& state = *impl_;

  // The readiness line is the first thing the process writes and it is flushed
  // immediately: a supervisor reads exactly one line to learn the identity and
  // the incarnation of the worker it just started.
  std::fputs(ready_line_.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);

  auto socket = Socket::connect_loopback(state.options.controller_port, state.options.connect_timeout);
  if (!socket.ok()) {
    return socket.status();
  }
  (void)socket.value().set_no_delay(true);
  state.channel = FrameChannel(std::move(socket).value(), state.options.limits);

  HelloMessage hello;
  hello.protocol_version = kProtocolVersion;
  hello.abi_version = kRuntimeAbiVersion;
  hello.worker = worker_id_;
  hello.incarnation = incarnation_;
  hello.name = state.options.name;
  hello.capacity = state.options.capacity;
  hello.started_at_unix_nanos = wall_now().unix_nanos();
  const Status sent = queue_message(state.channel, MessageType::kHello, state.next_sequence(),
                                    encode(hello), state.options.limits);
  if (!sent.ok()) {
    state.channel.close();
    return sent;
  }
  const Status flushed = state.channel.flush_out(state.options.connect_timeout);
  if (!flushed.ok()) {
    state.channel.close();
    return flushed;
  }

  // Handshake: the worker serves nothing until the controller has accepted it
  // and told it which incarnation and ABI it is talking to.
  const auto handshake_started = SteadyClock::now();
  const auto handshake_budget =
      std::chrono::nanoseconds(state.options.connect_timeout.nanos() > 0
                                   ? state.options.connect_timeout.nanos()
                                   : 0);
  bool acknowledged = false;
  while (!acknowledged) {
    const auto elapsed = SteadyClock::now() - handshake_started;
    if (elapsed >= handshake_budget) {
      state.channel.close();
      return make_status(StatusCode::kDeadlineExceeded,
                         "the controller did not acknowledge the worker's hello in time");
    }
    (void)state.channel.pump_in();
    if (!state.channel.failure().ok()) {
      const Status failure = state.channel.failure();
      state.channel.close();
      return failure;
    }
    std::vector<Frame> frames = state.channel.take_inbound();
    for (const Frame& frame : frames) {
      if (frame.type == MessageType::kHelloAck) {
        auto ack = decode_hello_ack(frame.body, state.options.limits);
        if (!ack.ok()) {
          state.channel.close();
          return ack.status();
        }
        if (ack.value().protocol_version != kProtocolVersion) {
          state.channel.close();
          return make_status(StatusCode::kNotSupported,
                             "the controller speaks protocol version " +
                                 std::to_string(ack.value().protocol_version));
        }
        if (ack.value().abi_version != kRuntimeAbiVersion) {
          state.channel.close();
          return make_status(StatusCode::kNotSupported,
                             "the controller speaks ABI version " +
                                 std::to_string(ack.value().abi_version) +
                                 " and this worker was built for " +
                                 std::to_string(kRuntimeAbiVersion));
        }
        state.controller_incarnation = ack.value().controller_incarnation;
        state.controller_epoch = ack.value().controller_epoch;
        if (ack.value().max_in_flight > 0) {
          state.capacity = std::min(state.capacity, ack.value().max_in_flight);
        }
        acknowledged = true;
      } else if (frame.type == MessageType::kError) {
        auto error = decode_error(frame.body, state.options.limits);
        const StatusCode code = error.ok() ? error.value().code : StatusCode::kInternal;
        const std::string detail =
            error.ok() ? error.value().detail : std::string("the controller refused the worker");
        state.channel.close();
        return make_status(code, detail);
      } else {
        state.channel.close();
        return make_status(StatusCode::kCorrupt,
                           "the controller sent " + std::string(to_string(frame.type)) +
                               " before acknowledging the worker's hello");
      }
    }
    if (!acknowledged) {
      SelectResult selected{};
      const Status waited = select_one(state.channel.socket(), false,
                                       Duration::from_millis(kReactorSliceMillis), selected);
      if (!waited.ok()) {
        state.channel.close();
        return waited;
      }
      if (selected.failed) {
        state.channel.close();
        return make_status(StatusCode::kUnavailable,
                           "the controller connection failed during the handshake");
      }
    }
  }

  // Serve. Nothing above this point accepts work and nothing below it mints an
  // identifier from another thread.
  auto last_heartbeat = SteadyClock::now();
  const auto heartbeat_period = std::chrono::nanoseconds(
      state.options.heartbeat_interval.nanos() > 0 ? state.options.heartbeat_interval.nanos()
                                                   : 0);
  Status outcome = Status::success();
  for (;;) {
    (void)state.channel.pump_out();
    if (!state.channel.failure().ok()) {
      outcome = state.channel.failure();
      break;
    }
    (void)state.channel.pump_in();
    if (!state.channel.failure().ok()) {
      outcome = state.channel.failure();
      break;
    }

    std::vector<Frame> frames = state.channel.take_inbound();
    for (const Frame& frame : frames) {
      Status handled = Status::success();
      switch (frame.type) {
        case MessageType::kDispatch:
          handled = state.handle_dispatch(frame);
          break;
        case MessageType::kCancel:
          handled = state.handle_cancel(frame);
          break;
        case MessageType::kReconcileRequest:
          handled = state.handle_reconcile(frame);
          break;
        case MessageType::kHealthRequest:
          handled = state.handle_health(frame);
          break;
        case MessageType::kHeartbeatAck:
          handled = state.handle_heartbeat_ack(frame);
          break;
        case MessageType::kShutdown: {
          auto shutdown = decode_shutdown(frame.body, state.options.limits);
          if (!shutdown.ok()) {
            handled = shutdown.status();
          } else {
            state.shutting_down = true;
          }
          break;
        }
        case MessageType::kError: {
          auto error = decode_error(frame.body, state.options.limits);
          handled = error.ok() ? make_status(error.value().code, error.value().detail)
                               : error.status();
          break;
        }
        default:
          handled = make_status(StatusCode::kCorrupt,
                                "a worker does not accept " +
                                    std::string(to_string(frame.type)) + " from a controller");
          break;
      }
      if (!handled.ok()) {
        outcome = handled;
        break;
      }
    }
    if (!outcome.ok() || state.shutting_down) {
      break;
    }

    const Status drained = state.drain_completions();
    if (!drained.ok()) {
      outcome = drained;
      break;
    }
    if (!state.channel.failure().ok()) {
      outcome = state.channel.failure();
      break;
    }

    const auto now = SteadyClock::now();
    if (now - last_heartbeat >= heartbeat_period) {
      last_heartbeat = now;
      HeartbeatMessage heartbeat;
      heartbeat.incarnation = incarnation_;
      heartbeat.in_flight = state.running_effects();
      heartbeat.completed = state.completed_attempts;
      heartbeat.failed = state.failed_attempts;
      heartbeat.at = wall_now();
      const Status beat = queue_message(state.channel, MessageType::kHeartbeat,
                                        state.next_sequence(), encode(heartbeat),
                                        state.options.limits);
      if (!beat.ok()) {
        outcome = beat;
        break;
      }
      (void)state.channel.pump_out();
      if (!state.channel.failure().ok()) {
        outcome = state.channel.failure();
        break;
      }
    }

    if (state.options.exit_after_attempts != 0 &&
        state.served_attempts >= state.options.exit_after_attempts) {
      break;
    }

    SelectResult selected{};
    const Status waited =
        select_one(state.channel.socket(), state.channel.outbound_backlog() > 0,
                   Duration::from_millis(kReactorSliceMillis), selected);
    if (!waited.ok()) {
      outcome = waited;
      break;
    }
    if (selected.failed) {
      outcome = make_status(StatusCode::kUnavailable, "the controller connection reported an error");
      break;
    }
  }

  // Shutdown order: stop admitting, stop and join every effect thread, then
  // close the socket. No completion is published after this point.
  state.shutting_down = true;
  state.accept_work = false;
  for (auto& entry : state.effects) {
    if (entry.second.stop != nullptr) {
      entry.second.stop->store(true, std::memory_order_relaxed);
    }
  }
  state.threads.clear();  // joins every effect thread
  (void)state.channel.flush_out(Duration::from_millis(kCrashFlushMillis));
  state.channel.close();
  return outcome;
}

}  // namespace rollout_fabric
