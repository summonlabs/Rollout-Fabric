// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The controller daemon: one process, one reactor thread, two listeners.
//
// Nothing inside a controller is concurrent. The journal, the orchestration
// state machine, the process-backed execution adapter and both listeners are
// driven by the thread that called run(); the concurrency this runtime proves
// lives in the worker processes on the other side of the loopback sockets.
//
// A restart is a new incarnation. Opening the journal takes exclusive ownership
// of the file, claims the next controller epoch, records the claim, and then
// reconstructs the inventory, the plans and the rollouts from the records that
// survive integrity checking. Anything the new incarnation cannot prove is
// reported rather than guessed at, and no attempt that was outstanding when the
// previous incarnation died is treated as failed - it is reconciled against the
// executor that may still be running it.
#include "rollout_fabric/controller.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "rollout_fabric/digest.hpp"
#include "rollout_fabric/json.hpp"
#include "rollout_fabric/persistence.hpp"
#include "rollout_fabric/report.hpp"
#include "rollout_fabric/version.hpp"

namespace rollout_fabric {
namespace {

// Bounds on what the reactor will do in one step. The reactor never blocks, so
// every loop it runs is explicitly capped.
constexpr std::size_t kMaxWorkerAcceptsPerStep = 1;
constexpr std::size_t kMaxControlAcceptsPerStep = 4;
constexpr std::size_t kMaxControlConnections = 64;
constexpr std::int64_t kControlFlushMillis = 250;
constexpr std::int64_t kControlWaitSliceMillis = 20;
constexpr std::uint64_t kMaxDocumentBytes = 1u << 20;

[[nodiscard]] std::uint32_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint32_t>(::getpid());
#endif
}

[[nodiscard]] std::string short_hex(const RolloutId& id) { return id.to_hex().substr(0, 12); }

[[nodiscard]] std::string_view adapter_event_kind_text(AdapterEventKind kind) noexcept {
  switch (kind) {
    case AdapterEventKind::kWorkerConnected: return "worker_connected";
    case AdapterEventKind::kWorkerDisconnected: return "worker_disconnected";
    case AdapterEventKind::kDispatchRefused: return "dispatch_refused";
    case AdapterEventKind::kProtocolError: return "protocol_error";
    case AdapterEventKind::kWorkerIncarnationChanged: return "worker_incarnation_changed";
  }
  return "unknown";
}

// One adapter event per line, in the order the adapter reported them, so a
// rendering is reproducible and an operator can diff two runs.
[[nodiscard]] std::string render_adapter_event(const AdapterEvent& event) {
  std::string text(event.at.to_rfc3339());
  text.append(" worker=").append(event.worker.to_hex().substr(0, 12));
  text.append(" incarnation=").append(event.incarnation.to_hex().substr(0, 12));
  text.append(" kind=").append(adapter_event_kind_text(event.kind));
  text.append(" detail=").append(event.detail);
  return text;
}

// Operator identity is derived from the name the request carried, so the same
// operator always maps to the same identifier and no request can forge another
// operator's identity by choosing a different one.
[[nodiscard]] OperatorId derive_operator_id(std::string_view name) noexcept {
  Sha256 hasher;
  hasher.update(std::string_view("rollout-fabric/operator/v1"));
  hasher.update(name);
  const Digest256 digest = hasher.finish();
  return OperatorId::from_bytes(
      std::span<const std::byte, OperatorId::byte_size>(digest.data(), OperatorId::byte_size));
}

// --- bounded document reading ----------------------------------------------

#if defined(_WIN32)
[[nodiscard]] Result<std::string> read_text_file(const std::string& path, std::uint64_t max_bytes) {
  if (path.empty()) {
    return make_status(StatusCode::kInvalidArgument, "the document path is empty");
  }
  const int needed = ::MultiByteToWideChar(CP_UTF8, 0, path.data(),
                                           static_cast<int>(path.size()), nullptr, 0);
  if (needed <= 0) {
    return make_status(StatusCode::kInvalidArgument, "the document path is not valid UTF-8");
  }
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wide.data(),
                            needed) != needed) {
    return make_status(StatusCode::kInvalidArgument, "the document path is not valid UTF-8");
  }
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return make_status(StatusCode::kNotFound, "the document '" + path + "' does not exist");
    }
    return make_status(StatusCode::kIoError,
                       "cannot open the document: Windows error " + std::to_string(error));
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(handle, &size) == 0) {
    (void)::CloseHandle(handle);
    return make_status(StatusCode::kIoError, "cannot determine the document size");
  }
  if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
    (void)::CloseHandle(handle);
    return make_status(StatusCode::kLimitExceeded,
                       "the document is larger than the configured ceiling of " +
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
      return make_status(StatusCode::kIoError, "cannot read the document: Windows error " +
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
    return make_status(StatusCode::kNotFound, "the document '" + path + "' does not exist");
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > max_bytes) {
    return make_status(StatusCode::kLimitExceeded, "the document is larger than the ceiling");
  }
  stream.seekg(0, std::ios::beg);
  std::string text(static_cast<std::size_t>(size), '\0');
  stream.read(text.data(), size);
  return text;
}
#endif

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct Controller::Impl {
  struct ControlSession {
    std::unique_ptr<FrameChannel> channel{};
    Timestamp accepted_at{};
  };

  ControllerOptions options{};
  SystemClock clock{};
  IdFactory ids{};
  std::unique_ptr<JournalWriter> journal{};
  TargetInventory inventory{};
  std::map<PlanId, ChangePlan> plans{};
  std::vector<Rollout> recovered{};
  std::unique_ptr<ProcessAdapter> adapter{};
  std::unique_ptr<Orchestrator> orchestrator{};
  Socket worker_listener{};
  Socket control_listener{};
  std::vector<ControlSession> control_clients{};
  IncarnationId incarnation{};
  EpochCounter controller_epoch{0};
  std::string stop_reason{};
  std::uint64_t commands = 0;
  bool opened = false;

  void accept_worker_connection() noexcept;
  void accept_control_connections();
  void serve_control_clients();
  void finalize();
  [[nodiscard]] CommandContext command_context(const CommandRequestMessage& request) const;
  [[nodiscard]] CommandResponseMessage execute_command(const CommandRequestMessage& request);
  [[nodiscard]] Status load_documents();
};

// --- reactor: workers -------------------------------------------------------

void Controller::Impl::accept_worker_connection() noexcept {
  for (std::size_t taken = 0; taken < kMaxWorkerAcceptsPerStep; ++taken) {
    auto accepted = worker_listener.accept();
    if (!accepted.ok()) {
      return;  // nothing pending, or a listener error the next step will see again
    }
    // The adapter owns the socket from here. A refusal (the worker ceiling) is
    // already reflected in its stats.
    (void)adapter->attach(std::move(accepted).value());
  }
}

// --- reactor: control clients ----------------------------------------------

void Controller::Impl::accept_control_connections() {
  for (std::size_t taken = 0; taken < kMaxControlAcceptsPerStep; ++taken) {
    auto accepted = control_listener.accept();
    if (!accepted.ok()) {
      return;
    }
    if (control_clients.size() >= std::min<std::size_t>(kMaxControlConnections,
                                                        options.limits.max_connections)) {
      // Refuse politely: the client learns that the controller is saturated
      // instead of waiting forever for an answer that will not come.
      Socket socket = std::move(accepted).value();
      FrameChannel channel(std::move(socket), options.limits);
      ErrorMessage error;
      error.code = StatusCode::kResourceExhausted;
      error.detail = "the controller is already serving its maximum number of control clients";
      (void)channel.queue(make_frame(MessageType::kError, Sequence{1}, encode(error)));
      (void)channel.flush_out(Duration::from_millis(kControlFlushMillis));
      channel.close();
      continue;
    }
    ControlSession session;
    session.accepted_at = clock.wall_now();
    session.channel = std::make_unique<FrameChannel>(std::move(accepted).value(), options.limits);
    control_clients.push_back(std::move(session));
  }
}

void Controller::Impl::serve_control_clients() {
  const Duration idle_timeout =
      Duration::from_millis(options.limits.connection_idle_timeout_millis);
  for (std::size_t index = 0; index < control_clients.size();) {
    ControlSession& session = control_clients[index];
    FrameChannel& channel = *session.channel;
    if (channel.closed() || !channel.failure().ok()) {
      control_clients.erase(control_clients.begin() + static_cast<std::ptrdiff_t>(index));
      continue;
    }
    (void)channel.pump_out();
    if (!channel.failure().ok()) {
      control_clients.erase(control_clients.begin() + static_cast<std::ptrdiff_t>(index));
      continue;
    }
    (void)channel.pump_in();
    if (!channel.failure().ok()) {
      control_clients.erase(control_clients.begin() + static_cast<std::ptrdiff_t>(index));
      continue;
    }

    std::vector<Frame> frames = channel.take_inbound();
    if (frames.empty()) {
      if (clock.wall_now() - session.accepted_at > idle_timeout) {
        channel.close();
        control_clients.erase(control_clients.begin() + static_cast<std::ptrdiff_t>(index));
        continue;
      }
      ++index;
      continue;
    }

    // Exactly one request, exactly one answer, then the connection is closed. A
    // control session carries no state, which is what makes the client side a
    // plain request/response exchange.
    const auto queue_error = [&](StatusCode code, const std::string& detail) {
      ErrorMessage error;
      error.code = code;
      error.detail = detail;
      const std::vector<std::byte> body = encode(error);
      if (body.size() <= options.limits.max_frame_bytes) {
        (void)channel.queue(make_frame(MessageType::kError, Sequence{1}, body));
      }
    };
    const Frame& frame = frames.front();
    if (frame.type == MessageType::kCommandRequest) {
      auto decoded = decode_command_request(frame.body, options.limits);
      if (!decoded.ok()) {
        queue_error(decoded.status().code(), decoded.status().message());
      } else {
        const CommandResponseMessage answer = execute_command(decoded.value());
        const std::vector<std::byte> body = encode(answer);
        if (body.size() <= options.limits.max_frame_bytes) {
          (void)channel.queue(make_frame(MessageType::kCommandResponse, Sequence{1}, body));
        }
      }
    } else {
      queue_error(StatusCode::kInvalidArgument,
                  "a control connection carries exactly one command request");
    }
    (void)channel.flush_out(Duration::from_millis(kControlFlushMillis));
    channel.close();
    control_clients.erase(control_clients.begin() + static_cast<std::ptrdiff_t>(index));
  }
}

void Controller::Impl::finalize() {
  if (orchestrator != nullptr) {
    orchestrator->note_shutdown();
  }
  if (adapter != nullptr) {
    (void)adapter->request_shutdown(stop_reason.empty() ? std::string("the controller is stopping")
                                                        : stop_reason);
    adapter->close_all();
  }
  for (ControlSession& session : control_clients) {
    if (session.channel != nullptr) {
      session.channel->close();
    }
  }
  control_clients.clear();
  (void)worker_listener.close();
  (void)control_listener.close();
  journal.reset();  // flushes and releases the exclusive lock
  shutdown_socket_runtime();
}

// ---------------------------------------------------------------------------
// Controller: construction and open
// ---------------------------------------------------------------------------

Controller::Controller() = default;
Controller::~Controller() = default;

Orchestrator& Controller::orchestrator() noexcept { return *impl_->orchestrator; }
const Orchestrator& Controller::orchestrator() const noexcept { return *impl_->orchestrator; }

Result<std::unique_ptr<Controller>> Controller::create(ControllerOptions options) {
  const Status limits_status = options.limits.validate();
  if (!limits_status.ok()) {
    return limits_status;
  }
  if (options.journal_path.empty()) {
    return make_status(StatusCode::kInvalidArgument, "ControllerOptions::journal_path is required");
  }
  if (options.worker_port != 0 && options.worker_port == options.control_port) {
    return make_status(StatusCode::kInvalidArgument,
                       "the worker listener and the control listener cannot share a port");
  }
  auto controller = std::unique_ptr<Controller>(new Controller());
  controller->impl_ = std::make_unique<Impl>();
  controller->impl_->options = std::move(options);
  controller->impl_->ids = IdFactory::from_entropy();
  return controller;
}

Status Controller::open() {
  if (impl_ == nullptr) {
    return make_status(StatusCode::kInternal, "the controller was not created");
  }
  Impl& state = *impl_;
  if (state.opened) {
    return make_status(StatusCode::kAlreadyExists, "this controller is already open");
  }
  const RuntimeLimits& limits = state.options.limits;

  // 1. Exclusive ownership of the journal. A second controller on the same
  //    journal fails here, with the operating system enforcing it.
  JournalOpenOptions journal_options;
  journal_options.path = state.options.journal_path;
  journal_options.limits = limits;
  journal_options.create_if_missing = true;
  journal_options.recover_truncated_tail = true;
  journal_options.exclusive = true;
  auto writer = JournalWriter::open(journal_options);
  if (!writer.ok()) {
    return writer.status();
  }
  state.journal = std::move(writer).value();

  // 2. Replay. The exclusive lock is already held, so nothing can append while
  //    this scan runs.
  std::vector<JournalRecord> records;
  auto scanned = scan_journal(state.options.journal_path, limits, &records, false);
  if (!scanned.ok()) {
    return scanned.status();
  }
  recovery_.journal = scanned.value();

  // 3. Claim the next epoch and record the claim before anything else happens,
  //    so an operator can see which incarnation owns the journal and when.
  EpochCounter highest{0};
  for (const JournalRecord& record : records) {
    if (value_of(record.controller_epoch) > value_of(highest)) {
      highest = record.controller_epoch;
    }
  }
  state.controller_epoch = EpochCounter{value_of(highest) + 1};
  state.incarnation = state.ids.next<IncarnationId>(IdDomain::kIncarnation);
  state.journal->set_writer_identity(state.incarnation, state.controller_epoch);
  const Timestamp claimed_at = state.clock.wall_now();
  const Status claim = state.journal->append(
      JournalRecordType::kIncarnationClaimed, state.controller_epoch, state.incarnation, claimed_at,
      encode_incarnation_claim(state.incarnation, state.controller_epoch, claimed_at));
  if (!claim.ok()) {
    return claim;
  }

  // 4. Reconstruct what the journal can prove: the inventory, the plans and the
  //    newest snapshot of every rollout.
  std::map<RolloutId, Rollout> rollouts;
  for (const JournalRecord& record : records) {
    switch (record.type) {
      case JournalRecordType::kInventoryStored: {
        auto decoded = decode_inventory_payload(record.payload, limits);
        if (!decoded.ok()) {
          return make_status(decoded.status().code(),
                             "the journal holds an inventory that cannot be read: " +
                                 decoded.status().message());
        }
        state.inventory = std::move(decoded).value();
        break;
      }
      case JournalRecordType::kPlanStored: {
        auto decoded = decode_plan_payload(record.payload, limits);
        if (!decoded.ok()) {
          return make_status(decoded.status().code(),
                             "the journal holds a plan that cannot be read: " +
                                 decoded.status().message());
        }
        ChangePlan plan = std::move(decoded).value();
        state.plans[plan.id] = std::move(plan);
        break;
      }
      case JournalRecordType::kRolloutCreated:
      case JournalRecordType::kRolloutSnapshot: {
        auto decoded = decode_rollout_payload(record.payload, limits);
        if (!decoded.ok()) {
          return make_status(decoded.status().code(),
                             "the journal holds a rollout snapshot that cannot be read: " +
                                 decoded.status().message());
        }
        Rollout rollout = std::move(decoded).value();
        // A later snapshot of the same rollout supersedes an earlier one; the
        // journal is append-only, so the last record wins.
        rollouts[rollout.id] = std::move(rollout);
        break;
      }
      default:
        break;
    }
  }

  // 5. Bind both loopback listeners. Nothing can connect until both exist, and
  //    the ports they actually got are what the readiness line reports.
  auto worker_listener = Socket::listen_loopback(state.options.worker_port);
  if (!worker_listener.ok()) {
    return worker_listener.status();
  }
  state.worker_listener = std::move(worker_listener).value();
  const Status worker_mode = state.worker_listener.set_nonblocking(true);
  if (!worker_mode.ok()) {
    return worker_mode;
  }
  auto control_listener = Socket::listen_loopback(state.options.control_port);
  if (!control_listener.ok()) {
    return control_listener.status();
  }
  state.control_listener = std::move(control_listener).value();
  const Status control_mode = state.control_listener.set_nonblocking(true);
  if (!control_mode.ok()) {
    return control_mode;
  }
  status_.worker_port = state.worker_listener.bound_port();
  status_.control_port = state.control_listener.bound_port();

  // 6. The execution plane, then the state machine that drives it, then the
  //    binding between them. All three run on this one thread.
  ProcessAdapterOptions adapter_options;
  adapter_options.limits = limits;
  adapter_options.connect_timeout = Duration::from_seconds(5);
  adapter_options.silence_timeout =
      Duration::from_millis(limits.connection_idle_timeout_millis);
  adapter_options.max_workers = limits.max_workers;
  state.adapter = std::make_unique<ProcessAdapter>(state.clock, adapter_options);
  state.adapter->set_health_source(state.ids.next<HealthSourceId>(IdDomain::kHealthSource));

  OrchestratorDeps deps;
  deps.limits = limits;
  deps.clock = &state.clock;
  deps.journal = state.journal.get();
  deps.execution = state.adapter.get();
  deps.health = state.adapter.get();
  deps.ids = IdFactory::from_entropy();
  deps.incarnation = state.incarnation;
  deps.controller_epoch = state.controller_epoch;
  deps.inventory = state.inventory;
  state.orchestrator = std::make_unique<Orchestrator>(std::move(deps));
  state.adapter->set_sink(state.orchestrator.get());

  std::string skipped;
  for (const auto& entry : rollouts) {
    const Rollout& rollout = entry.second;
    const bool stale = !(rollout.incarnation == state.incarnation);
    const auto plan = state.plans.find(rollout.plan_id);
    if (plan == state.plans.end()) {
      ++recovery_.rollouts_skipped;
      skipped.append(skipped.empty() ? "" : "; ");
      skipped.append("rollout ").append(short_hex(rollout.id))
          .append(" has no stored plan and was not adopted");
      continue;
    }
    const Status restored =
        state.orchestrator->restore(rollout, plan->second, recovery_.journal);
    if (!restored.ok()) {
      ++recovery_.rollouts_skipped;
      skipped.append(skipped.empty() ? "" : "; ");
      skipped.append("rollout ").append(short_hex(rollout.id))
          .append(" was not adopted: ").append(restored.message());
      continue;
    }
    ++recovery_.rollouts_restored;
    recovery_.outstanding_attempts += rollout.outstanding_attempts;
    if (stale) {
      ++recovery_.stale_incarnations;
    }
    state.recovered.push_back(rollout);
  }
  if (!skipped.empty()) {
    recovery_.detail = std::move(skipped);
  }

  status_.incarnation = state.incarnation;
  status_.controller_epoch = state.controller_epoch;
  status_.rollouts = static_cast<std::uint32_t>(state.orchestrator->rollouts().size());

  // 7. Exactly one readiness line, flushed, before the controller is reachable.
  std::string ready = "rolloutd ready incarnation=";
  ready.append(state.incarnation.to_hex());
  ready.append(" epoch=").append(std::to_string(value_of(state.controller_epoch)));
  ready.append(" control_port=").append(std::to_string(status_.control_port));
  ready.append(" worker_port=").append(std::to_string(status_.worker_port));
  ready.append(" pid=").append(std::to_string(current_process_id()));
  std::fputs(ready.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);

  // 8. Operator supplied documents, if any, are applied through the ordinary
  //    command API so that a start is indistinguishable from an operator's.
  const Status documents = state.load_documents();
  if (!documents.ok()) {
    return documents;
  }
  status_.rollouts = static_cast<std::uint32_t>(state.orchestrator->rollouts().size());
  state.opened = true;
  return Status::success();
}

Status Controller::Impl::load_documents() {
  const RuntimeLimits& limits = options.limits;
  if (!options.inventory_path.empty()) {
    auto text = read_text_file(options.inventory_path, kMaxDocumentBytes);
    if (!text.ok()) {
      return text.status();
    }
    auto loaded = parse_inventory(text.value(), limits);
    if (!loaded.ok()) {
      return loaded.status();
    }
    const Status applied = orchestrator->set_inventory(loaded.value());
    if (!applied.ok()) {
      return applied;
    }
    this->inventory = std::move(loaded).value();
  }

  if (options.plan_path.empty()) {
    return Status::success();
  }
  auto text = read_text_file(options.plan_path, kMaxDocumentBytes);
  if (!text.ok()) {
    return text.status();
  }
  auto plan = parse_change_plan(text.value(), limits);
  if (!plan.ok()) {
    return plan.status();
  }

  for (const Rollout* existing : orchestrator->rollouts()) {
    if (existing->plan_id == plan.value().id) {
      // This document is already executing: adopting the recovered rollout is
      // the whole point of a restart, and creating a second one would be a
      // second change of the same plan.
      return Status::success();
    }
  }

  CommandContext context;
  context.authority = options.authority;
  context.operator_id = derive_operator_id("rolloutd");
  context.operator_name = "rolloutd";
  context.incarnation = incarnation;
  context.controller_epoch = controller_epoch;

  std::string detail;
  auto created = orchestrator->create_rollout(plan.value(), context, &detail);
  if (!created.ok()) {
    return created.status();
  }
  const RolloutId id = created.value();
  if (options.auto_arm) {
    const Status armed = orchestrator->arm(id, context, &detail);
    if (!armed.ok()) {
      return armed;
    }
  }
  if (options.auto_start) {
    const Status started = orchestrator->start(id, context, &detail);
    if (!started.ok()) {
      return started;
    }
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// Controller: reactor
// ---------------------------------------------------------------------------

Status Controller::step() {
  if (impl_ == nullptr) {
    return make_status(StatusCode::kInternal, "the controller was not created");
  }
  if (!impl_->opened) {
    return make_status(StatusCode::kPreconditionFailed, "the controller has not been opened");
  }
  if (stop_requested_) {
    // Stop means stop: no connection is accepted, no frame is produced and no
    // attempt is dispatched after this point.
    return make_status(StatusCode::kShuttingDown,
                       "a stop has been requested, so no further work is issued");
  }
  Impl& state = *impl_;

  state.accept_worker_connection();
  state.accept_control_connections();
  state.serve_control_clients();

  // Move the workers' bytes, hand the evidence they produced to the state
  // machine, and then take exactly one orchestration step.
  state.adapter->poll(*state.orchestrator);
  (void)state.orchestrator->tick();

  status_.ticks += 1;
  status_.commands = state.commands;
  status_.rollouts = static_cast<std::uint32_t>(state.orchestrator->rollouts().size());
  status_.evidence_accepted = state.orchestrator->counters().evidence_accepted;
  status_.evidence_rejected = state.orchestrator->counters().evidence_rejected;
  status_.workers_attached = static_cast<std::uint64_t>(state.adapter->attached_count());

  const Status& storage = state.orchestrator->storage_status();
  if (!storage.ok()) {
    return make_status(storage.code(),
                       "the orchestration state could not be persisted: " + storage.message());
  }
  return Status::success();
}

Status Controller::run() {
  if (impl_ == nullptr) {
    return make_status(StatusCode::kInternal, "the controller was not created");
  }
  Impl& state = *impl_;
  Status outcome = Status::success();
  while (!stop_requested_) {
    const Status stepped = step();
    if (!stepped.ok()) {
      if (stepped.code() == StatusCode::kShuttingDown) {
        break;
      }
      outcome = stepped;
      break;
    }
    // A short, bounded wait on the listeners keeps the loop responsive without
    // ever blocking on a peer.
    Duration slice = state.options.tick_interval;
    if (slice.is_zero() || slice.is_negative()) {
      slice = Duration::from_millis(1);
    }
    const Duration half = Duration::from_nanos(slice.nanos() / 2);
    SelectResult worker_ready{};
    (void)select_one(state.worker_listener, false, half.is_zero() ? slice : half, worker_ready);
    if (!stop_requested_) {
      SelectResult control_ready{};
      (void)select_one(state.control_listener, false, half.is_zero() ? slice : half, control_ready);
    }
  }
  state.finalize();
  return outcome;
}

void Controller::request_stop(std::string reason) {
  stop_requested_ = true;
  if (impl_ != nullptr) {
    impl_->stop_reason = std::move(reason);
  }
}

// ---------------------------------------------------------------------------
// Controller: commands
// ---------------------------------------------------------------------------

CommandContext Controller::Impl::command_context(const CommandRequestMessage& request) const {
  CommandContext context;
  context.authority = options.authority;
  context.operator_id = derive_operator_id(request.operator_name);
  context.operator_name = request.operator_name;
  context.incarnation = incarnation;
  context.controller_epoch = controller_epoch;
  context.expected_generation = request.generation;
  context.expected_revision = request.revision;
  context.checked_generation = request.checked_generation;
  context.checked_revision = request.checked_revision;
  return context;
}

CommandResponseMessage Controller::Impl::execute_command(const CommandRequestMessage& request) {
  const RuntimeLimits& limits = options.limits;
  CommandContext context = command_context(request);
  CommandResponseMessage response;
  response.rollout = request.rollout;
  response.gate = request.gate;
  ++commands;

  // Every mutating command shares this shape: ask the state machine, then render
  // the state that resulted. The detail the state machine produced is what the
  // operator is told.
  const auto report = [&](const Status& status, std::string detail) {
    response.code = status.code();
    if (!detail.empty()) {
      response.detail = std::move(detail);
    } else {
      response.detail = status.message();
    }
    if (status.ok() && !request.rollout.is_nil()) {
      const Rollout* rollout = orchestrator->find(request.rollout);
      if (rollout != nullptr) {
        response.payload = render_status(*rollout, orchestrator->inventory(), limits);
      }
    }
    return response;
  };
  const auto missing_rollout = [&]() {
    response.code = StatusCode::kInvalidArgument;
    response.detail = "this command must name the rollout it applies to";
    return response;
  };

  switch (request.kind) {
    case CommandKind::kCreate: {
      if (request.plan_path.empty()) {
        response.code = StatusCode::kInvalidArgument;
        response.detail = "kCreate must name the plan document to load";
        return response;
      }
      auto text = read_text_file(request.plan_path, kMaxDocumentBytes);
      if (!text.ok()) {
        response.code = text.status().code();
        response.detail = text.status().message();
        return response;
      }
      auto plan = parse_change_plan(text.value(), limits);
      if (!plan.ok()) {
        response.code = plan.status().code();
        response.detail = plan.status().message();
        return response;
      }
      std::string detail;
      auto created = orchestrator->create_rollout(plan.value(), context, &detail);
      if (!created.ok()) {
        response.code = created.status().code();
        response.detail = detail.empty() ? created.status().message() : detail;
        return response;
      }
      response.rollout = created.value();
      response.code = StatusCode::kOk;
      response.detail = detail;
      const Rollout* rollout = orchestrator->find(created.value());
      if (rollout != nullptr) {
        response.payload = render_status(*rollout, orchestrator->inventory(), limits);
      }
      return response;
    }
    case CommandKind::kValidate: {
      if (request.rollout.is_nil()) {
        return missing_rollout();
      }
      const Rollout* rollout = orchestrator->find(request.rollout);
      if (rollout == nullptr) {
        response.code = StatusCode::kNotFound;
        response.detail = "no rollout " + short_hex(request.rollout) + " is known to this controller";
        return response;
      }
      // Validation happens when the plan is loaded and is recorded as the
      // rollout's first decision; this command reports its durable result
      // instead of re-running it against a document the controller no longer
      // holds.
      response.code = StatusCode::kOk;
      response.detail = "the plan of rollout " + short_hex(rollout->id) +
                        " was validated when the rollout was created; the rollout is " +
                        std::string(to_string(rollout->state));
      response.payload = render_status(*rollout, orchestrator->inventory(), limits);
      return response;
    }
    case CommandKind::kArm: {
      if (request.rollout.is_nil()) {
        return missing_rollout();
      }
      std::string detail;
      const Status status = orchestrator->arm(request.rollout, context, &detail);
      return report(status, std::move(detail));
    }
    case CommandKind::kStart: {
      if (request.rollout.is_nil()) {
        return missing_rollout();
      }
      std::string detail;
      const Status status = orchestrator->start(request.rollout, context, &detail);
      return report(status, std::move(detail));
    }
    case CommandKind::kPause: {
      if (request.rollout.is_nil()) {
        return missing_rollout();
      }
      std::string detail;
      const Status status =
          orchestrator->pause(request.rollout, context, request.reason, &detail);
      return report(status, std::move(detail));
    }
    case CommandKind::kResume: {
      if (request.rollout.is_nil()) {
        return missing_rollout();
      }
      std::string detail;
      const Status status = orchestrator->resume(request.rollout, context, &detail);
      return report(status, std::move(detail));
    }
    case CommandKind::kAbort: {
      if (request.rollout.is_nil()) {
        return missing_rollout();
      }
      std::string detail;
      const Status status =
          orchestrator->abort(request.rollout, context, request.reason, &detail);
      return report(status, std::move(detail));
    }
    case CommandKind::kApproveGate: {
      if (request.rollout.is_nil()) {
        return missing_rollout();
      }
      if (request.gate.is_nil()) {
        response.code = StatusCode::kInvalidArgument;
        response.detail = "kApproveGate must name the gate being approved";
        return response;
      }
      std::string detail;
      const Status status = orchestrator->approve_gate(request.rollout, request.gate, context, &detail);
      return report(status, std::move(detail));
    }
    case CommandKind::kRetire: {
      if (request.rollout.is_nil()) {
        return missing_rollout();
      }
      std::string detail;
      const Status status = orchestrator->retire(request.rollout, context, &detail);
      return report(status, std::move(detail));
    }
    case CommandKind::kRegenerate: {
      if (request.rollout.is_nil()) {
        return missing_rollout();
      }
      std::string detail;
      const Status status = orchestrator->regenerate(request.rollout, context, &detail);
      return report(status, std::move(detail));
    }
    case CommandKind::kStatus: {
      if (!request.rollout.is_nil()) {
        const Rollout* rollout = orchestrator->find(request.rollout);
        if (rollout == nullptr) {
          response.code = StatusCode::kNotFound;
          response.detail = "no rollout " + short_hex(request.rollout) +
                            " is known to this controller";
          return response;
        }
        response.code = StatusCode::kOk;
        response.detail = "rollout " + short_hex(rollout->id) + " is " +
                          std::string(to_string(rollout->state));
        response.payload = render_status(*rollout, orchestrator->inventory(), limits);
        return response;
      }
      std::string listing;
      for (const Rollout* rollout : orchestrator->rollouts()) {
        listing.append(rollout->id.to_hex())
            .append(" state=")
            .append(to_string(rollout->state))
            .append(" plan=")
            .append(rollout->plan_id.to_hex().substr(0, 12))
            .append(" revision=")
            .append(std::to_string(value_of(rollout->revision)))
            .append("\n");
      }
      if (listing.empty()) {
        listing = "no rollout is known to this controller\n";
      }
      response.code = StatusCode::kOk;
      response.detail = std::to_string(orchestrator->rollouts().size()) + " rollout(s)";
      response.payload = std::move(listing);
      return response;
    }
    case CommandKind::kExplain: {
      if (request.rollout.is_nil()) {
        return missing_rollout();
      }
      std::optional<DecisionId> decision;
      if (!request.gate.is_nil()) {
        decision = DecisionId::from_bytes(request.gate.span());
      }
      auto explained = orchestrator->explain(request.rollout, decision);
      if (!explained.ok()) {
        response.code = explained.status().code();
        response.detail = explained.status().message();
        return response;
      }
      response.code = StatusCode::kOk;
      response.detail = decision.has_value() ? "the requested decision" : "the latest decision";
      response.payload = std::move(explained).value();
      return response;
    }
    case CommandKind::kInspectStage: {
      if (request.rollout.is_nil()) {
        return missing_rollout();
      }
      if (request.gate.is_nil()) {
        response.code = StatusCode::kInvalidArgument;
        response.detail = "kInspectStage carries the stage identifier in the gate field";
        return response;
      }
      auto view = orchestrator->stage_view(request.rollout, StageId::from_bytes(request.gate.span()));
      if (!view.ok()) {
        response.code = view.status().code();
        response.detail = view.status().message();
        return response;
      }
      response.code = StatusCode::kOk;
      response.detail = "stage " + view.value().id.to_hex().substr(0, 12) + " is " +
                        std::string(to_string(view.value().state));
      response.payload = render_stage(view.value(), orchestrator->inventory());
      return response;
    }
    case CommandKind::kInspectCohort: {
      if (request.rollout.is_nil()) {
        return missing_rollout();
      }
      if (request.gate.is_nil()) {
        response.code = StatusCode::kInvalidArgument;
        response.detail = "kInspectCohort carries the stage identifier in the gate field";
        return response;
      }
      auto view = orchestrator->cohort_view(request.rollout, StageId::from_bytes(request.gate.span()));
      if (!view.ok()) {
        response.code = view.status().code();
        response.detail = view.status().message();
        return response;
      }
      response.code = StatusCode::kOk;
      response.detail = std::to_string(view.value().members.size()) + " cohort member(s)";
      response.payload = render_cohort_membership(view.value(), orchestrator->inventory());
      return response;
    }
    case CommandKind::kListEvents: {
      std::string listing;
      for (const AdapterEvent& event : orchestrator->recent_adapter_events()) {
        listing.append(render_adapter_event(event)).append("\n");
      }
      if (listing.empty()) {
        listing = "no adapter event has been observed by this controller\n";
      }
      response.code = StatusCode::kOk;
      response.detail = std::to_string(orchestrator->recent_adapter_events().size()) + " event(s)";
      response.payload = std::move(listing);
      return response;
    }
    default:
      response.code = StatusCode::kNotSupported;
      response.detail = "this controller does not implement command " +
                        std::string(to_string(request.kind));
      return response;
  }
}

// ---------------------------------------------------------------------------
// ControlClient
// ---------------------------------------------------------------------------

ControlClient::~ControlClient() = default;
ControlClient::ControlClient(ControlClient&& other) noexcept = default;

Result<ControlClient> ControlClient::connect(std::uint16_t port, Duration connect_timeout,
                                             const RuntimeLimits& limits) {
  const Status limits_status = limits.validate();
  if (!limits_status.ok()) {
    return limits_status;
  }
  if (port == 0) {
    return make_status(StatusCode::kInvalidArgument, "a control client needs a port to connect to");
  }
  auto socket = Socket::connect_loopback(port, connect_timeout);
  if (!socket.ok()) {
    return socket.status();
  }
  const Status delay = socket.value().set_no_delay(true);
  if (!delay.ok()) {
    (void)socket.value().close();
    return delay;
  }
  ControlClient client;
  client.limits_ = limits;
  client.channel_ = std::make_unique<FrameChannel>(std::move(socket).value(), limits);
  if (!client.channel_->failure().ok()) {
    return client.channel_->failure();
  }
  return client;
}

Result<CommandResponseMessage> ControlClient::send(const CommandRequestMessage& request) {
  if (channel_ == nullptr) {
    return make_status(StatusCode::kUnavailable, "the control client is not connected");
  }
  if (channel_->closed() || !channel_->failure().ok()) {
    return make_status(StatusCode::kUnavailable, "the control connection is closed");
  }
  const std::vector<std::byte> body = encode(request);
  if (body.size() > limits_.max_frame_bytes) {
    return make_status(StatusCode::kLimitExceeded,
                       "the encoded command is larger than max_frame_bytes");
  }
  const Status queued =
      channel_->queue(make_frame(MessageType::kCommandRequest, Sequence{++sequence_}, body));
  if (!queued.ok()) {
    return queued;
  }

  const Duration budget = Duration::from_millis(limits_.connection_idle_timeout_millis);
  const Status flushed = channel_->flush_out(budget);
  if (!flushed.ok()) {
    return flushed;
  }

  const auto budget_nanos = std::chrono::nanoseconds(budget.nanos() > 0 ? budget.nanos() : 0);
  const auto started = std::chrono::steady_clock::now();
  for (;;) {
    (void)channel_->pump_in();
    if (!channel_->failure().ok()) {
      return channel_->failure();
    }
    std::vector<Frame> frames = channel_->take_inbound();
    if (!frames.empty()) {
      const Frame& frame = frames.front();
      if (frame.type == MessageType::kCommandResponse) {
        auto decoded = decode_command_response(frame.body, limits_);
        if (!decoded.ok()) {
          return decoded.status();
        }
        return std::move(decoded).value();
      }
      if (frame.type == MessageType::kError) {
        auto decoded = decode_error(frame.body, limits_);
        if (!decoded.ok()) {
          return decoded.status();
        }
        return make_status(decoded.value().code, decoded.value().detail);
      }
      return make_status(StatusCode::kCorrupt,
                         "the controller sent " + std::string(to_string(frame.type)) +
                             " in answer to a command request");
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (elapsed >= budget_nanos) {
      return make_status(StatusCode::kDeadlineExceeded,
                         "the controller did not answer the command within the connection idle "
                         "timeout");
    }
    Duration wait = Duration::from_nanos(
        std::chrono::duration_cast<std::chrono::nanoseconds>(budget_nanos - elapsed).count());
    if (wait > Duration::from_millis(kControlWaitSliceMillis)) {
      wait = Duration::from_millis(kControlWaitSliceMillis);
    }
    SelectResult selected{};
    const Status waited = select_one(channel_->socket(), false, wait, selected);
    if (!waited.ok()) {
      return waited;
    }
    if (selected.failed) {
      return make_status(StatusCode::kUnavailable,
                         "the control connection reported an error while waiting for an answer");
    }
  }
}

Status ControlClient::close() {
  if (channel_ == nullptr) {
    return Status::success();
  }
  (void)channel_->flush_out(Duration::from_millis(kControlFlushMillis));
  channel_->close();
  channel_.reset();
  return Status::success();
}

}  // namespace rollout_fabric
