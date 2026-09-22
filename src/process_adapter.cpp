// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The execution and health planes realised over real worker processes.
//
// One attached worker process is one duplex framed TCP connection. The adapter
// never invents a completion: it frames a dispatch, and when the frame reaches
// the transport the adapter reports acceptance - which proves only that the
// frame was handed over. The worker's acknowledgement and its evidence arrive
// later, as separate observations, and a connection that dies turns every
// attempt it carried into "unknown" so the orchestrator reconciles rather than
// guessing that an effect did not happen.
//
// Threading: everything here runs on the controller's single reactor thread.
// There is no lock in this file, there is no background thread, and the sink is
// only ever called from inside poll(), reconcile() or a supervision call made
// by that same thread.
#include "rollout_fabric/process_adapter.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "rollout_fabric/orchestrator.hpp"
#include "rollout_fabric/protocol.hpp"
#include "rollout_fabric/version.hpp"

namespace rollout_fabric {
namespace {

// The longest a single select() may block inside the bounded reconcile loop.
constexpr std::int64_t kReconcileSliceMillis = 20;
constexpr std::int64_t kShutdownFlushMillis = 250;

[[nodiscard]] std::string short_hex(const WorkerId& id) { return id.to_hex().substr(0, 12); }

[[nodiscard]] std::string short_hex(const AttemptId& id) { return id.to_hex().substr(0, 12); }

// The orchestration state machine that owns the attempts this adapter
// dispatches also installs itself as the evidence sink. Two facts the adapter
// cannot otherwise obtain live there: the controller identity a worker is told
// about, and the target an attempt belongs to. The adapter asks for them
// through the sink it was given and degrades to "not known" for any other sink.
[[nodiscard]] const Orchestrator* state_machine_of(IEvidenceSink* sink) noexcept {
  return dynamic_cast<const Orchestrator*>(sink);
}

struct ControllerIdentity {
  IncarnationId incarnation{};
  EpochCounter epoch{0};
};

[[nodiscard]] ControllerIdentity controller_identity(IEvidenceSink* sink) noexcept {
  ControllerIdentity identity;
  const Orchestrator* state = state_machine_of(sink);
  if (state != nullptr) {
    identity.incarnation = state->incarnation();
    identity.epoch = state->controller_epoch();
  }
  return identity;
}

// The target an attempt applies to, and a sequence value that is guaranteed to
// be ahead of everything already applied to that attempt. Reconciled evidence
// describes an attempt the state machine already holds, so both facts come from
// it rather than from a guess.
struct AttemptAnchor {
  bool known = false;
  TargetId target{};
  Sequence sequence{};
};

[[nodiscard]] AttemptAnchor anchor_attempt(IEvidenceSink* sink, const AttemptId& attempt) noexcept {
  AttemptAnchor anchor;
  const Orchestrator* state = state_machine_of(sink);
  if (state == nullptr) {
    return anchor;
  }
  const Attempt* live = state->find_attempt(attempt);
  if (live == nullptr) {
    return anchor;
  }
  anchor.known = true;
  anchor.target = live->target;
  anchor.sequence = Sequence{value_of(live->highest_sequence) + 1};
  return anchor;
}

void emit_event(IEvidenceSink* sink, AdapterEventKind kind, const WorkerId& worker,
                const IncarnationId& incarnation, std::string detail, Timestamp at) {
  if (sink == nullptr) {
    return;
  }
  AdapterEvent event;
  event.kind = kind;
  event.worker = worker;
  event.incarnation = incarnation;
  event.detail = std::move(detail);
  event.at = at;
  sink->on_adapter_event(std::move(event));
}

// Identifiers for the evidence this adapter attests to by itself (a reconcile
// answer it delivered). Unique across adapter instances in one process and
// deterministic in order of use within a run.
[[nodiscard]] IdFactory& adapter_ids() {
  static IdFactory factory = IdFactory::from_entropy();
  return factory;
}

[[nodiscard]] bool mentions(const std::vector<std::uint64_t>& ids, std::uint64_t id) noexcept {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

struct ProcessAdapter::Connection {
  FrameChannel channel{};
  WorkerId worker{};
  IncarnationId incarnation{};
  std::string name;
  std::uint32_t capacity = 0;
  std::uint32_t in_flight = 0;
  std::uint64_t completed = 0;
  std::uint64_t failed = 0;
  bool hello_received = false;
  // Kept current by drop_connection() so a route, a pending reconcile or a
  // frame handler can name its owner without searching the vector.
  std::size_t index = 0;
  Timestamp attached_at{};
  Timestamp last_activity{};

  [[nodiscard]] bool usable() const noexcept {
    return hello_received && !channel.closed() && channel.failure().ok();
  }

  // A protocol violation closes exactly this connection and nothing else: the
  // other workers keep serving, and the orchestrator is told which peer
  // offended.
  void protocol_failure(AdapterStats& stats, IEvidenceSink* sink, Timestamp at, Sequence sequence,
                        std::string what, const Status& status) {
    ++stats.protocol_errors;
    if (sink != nullptr) {
      AdapterEvent event;
      event.kind = AdapterEventKind::kProtocolError;
      event.worker = worker;
      event.incarnation = incarnation;
      event.detail = std::move(what) + ": " + status.to_string();
      event.at = at;
      sink->on_adapter_event(std::move(event));
    }
    if (!channel.closed()) {
      ErrorMessage error;
      error.code = status.code();
      error.detail = status.message();
      const std::vector<std::byte> body = encode(error);
      (void)channel.queue(make_frame(MessageType::kError, sequence, body));
      (void)channel.pump_out();
      channel.close();
    }
  }
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ProcessAdapter::ProcessAdapter(const Clock& clock, ProcessAdapterOptions options)
    : clock_(&clock), options_(std::move(options)) {
  if (options_.max_workers == 0) {
    options_.max_workers = 1;
  }
  if (options_.max_workers > options_.limits.max_workers) {
    options_.max_workers = options_.limits.max_workers;
  }
  if (options_.max_workers == 0) {
    options_.max_workers = 1;
  }
}

ProcessAdapter::~ProcessAdapter() { close_all(); }

Status ProcessAdapter::attach(Socket socket) {
  if (!socket.valid()) {
    return make_status(StatusCode::kInvalidArgument, "attach: the socket is not open");
  }
  if (connections_.size() >= options_.max_workers) {
    (void)socket.close();
    return make_status(StatusCode::kResourceExhausted,
                       "attach: the adapter already holds " + std::to_string(connections_.size()) +
                           " worker connection(s), at max_workers");
  }
  const Status delay = socket.set_no_delay(true);
  if (!delay.ok()) {
    (void)socket.close();
    return delay;
  }

  auto connection = std::make_unique<Connection>();
  connection->channel = FrameChannel(std::move(socket), options_.limits);
  connection->index = connections_.size();
  connection->attached_at = clock_->monotonic_now();
  connection->last_activity = connection->attached_at;
  const std::size_t index = connections_.size();
  connections_.push_back(std::move(connection));
  if (!connections_[index]->channel.failure().ok()) {
    const Status failure = connections_[index]->channel.failure();
    drop_connection(index, "the connection could not be prepared: " + failure.message(),
                    sink_hint_);
    return failure;
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// IExecutionAdapter
// ---------------------------------------------------------------------------

bool ProcessAdapter::available() const noexcept {
  for (const auto& connection : connections_) {
    if (connection->usable()) {
      return true;
    }
  }
  return false;
}

ProcessAdapter::Connection* ProcessAdapter::pick_worker(const TargetId& target) {
  // Worker selection is round-robin over the attachment order - a deterministic
  // order that never depends on map iteration. The target is part of the
  // signature because a future affinity rule belongs here; no affinity is
  // claimed today.
  (void)target;
  const std::size_t count = connections_.size();
  if (count == 0) {
    return nullptr;
  }
  const std::size_t start = static_cast<std::size_t>(
      stats_.dispatched % static_cast<std::uint64_t>(count));
  for (std::size_t step = 0; step < count; ++step) {
    Connection& connection = *connections_[(start + step) % count];
    if (!connection.usable()) {
      continue;
    }
    if (connection.in_flight >= connection.capacity) {
      continue;
    }
    return &connection;
  }
  return nullptr;
}

Status ProcessAdapter::send_to(Connection& connection, const Frame& frame) {
  if (connection.channel.closed()) {
    return make_status(StatusCode::kUnavailable, "the worker connection is closed");
  }
  if (!connection.channel.failure().ok()) {
    return connection.channel.failure();
  }
  const Status queued = connection.channel.queue(frame);
  if (!queued.ok()) {
    return queued;
  }
  stats_.bytes_out += static_cast<std::uint64_t>(frame.body.size()) + kFrameHeaderBytes;
  (void)connection.channel.pump_out();
  if (!connection.channel.failure().ok()) {
    return connection.channel.failure();
  }
  return Status::success();
}

DispatchResult ProcessAdapter::dispatch(const DispatchRequest& request) {
  DispatchResult result;
  ++stats_.dispatched;

  if (request.fence.attempt.is_nil()) {
    ++stats_.dispatch_refused;
    result.outcome = DispatchOutcome::kFenced;
    result.detail = "the dispatch names no attempt, so there would be nothing to attribute "
                    "evidence to";
    return result;
  }

  Connection* connection = pick_worker(request.target);
  if (connection == nullptr) {
    ++stats_.dispatch_refused;
    result.outcome = DispatchOutcome::kUnavailable;
    result.detail = available() ? "every attached worker is at its capacity"
                                : "no attached worker has completed its hello handshake";
    return result;
  }

  DispatchCommandMessage message;
  message.request = request;
  message.target = request.target;
  const Orchestrator* state = state_machine_of(sink_hint_);
  if (state != nullptr && !request.target.is_nil()) {
    const TargetDescriptor* descriptor = state->inventory().find(request.target);
    if (descriptor != nullptr) {
      message.target_name = descriptor->name;
    }
  }

  const Status queued = send_to(*connection,
                                make_frame(MessageType::kDispatch, next_sequence(),
                                           encode(message, options_.limits)));
  if (!queued.ok()) {
    ++stats_.dispatch_refused;
    if (queued.code() == StatusCode::kResourceExhausted) {
      result.outcome = DispatchOutcome::kThrottled;
      result.detail = "the outbound queue of worker " + short_hex(connection->worker) +
                      " is full; the dispatch was not queued";
      return result;
    }
    const WorkerId worker = connection->worker;
    const std::size_t index = connection->index;
    result.outcome = DispatchOutcome::kUnavailable;
    result.detail = "the connection to worker " + short_hex(worker) +
                    " failed: " + queued.message();
    drop_connection(index, "dispatch failed: " + queued.message(), sink_hint_);
    return result;
  }

  connection->in_flight += 1;
  attempt_routes_[request.fence.attempt] = connection->index;
  ++stats_.dispatch_accepted;
  result.outcome = DispatchOutcome::kAccepted;
  result.accepted_at = clock_->wall_now();
  result.detail = "accepted: the dispatch frame for attempt " +
                  short_hex(request.fence.attempt) + " was handed to the transport for worker " +
                  short_hex(connection->worker) +
                  ". Acceptance means the frame was queued, not that the effect happened: the "
                  "worker acknowledges separately and its completion arrives as its own "
                  "evidence";
  return result;
}

Status ProcessAdapter::cancel(const AttemptFence& fence) {
  const auto route = attempt_routes_.find(fence.attempt);
  if (route == attempt_routes_.end() || route->second >= connections_.size()) {
    return make_status(StatusCode::kUnavailable,
                       "no attached worker owns attempt " + short_hex(fence.attempt) +
                           ", so it cannot be asked to stop");
  }
  Connection& connection = *connections_[route->second];
  CancelMessage message;
  message.fence = fence;
  message.reason = "the controller asked this attempt to stop";
  return send_to(connection, make_frame(MessageType::kCancel, next_sequence(), encode(message)));
}

void ProcessAdapter::poll(IEvidenceSink& sink) {
  sink_hint_ = &sink;
  for (std::size_t index = 0; index < connections_.size();) {
    Connection& connection = *connections_[index];
    if (connection.channel.closed()) {
      drop_connection(index, "the connection was closed", &sink);
      continue;
    }
    (void)connection.channel.pump_out();
    if (!connection.channel.failure().ok()) {
      drop_connection(index, "the connection failed while flushing: " +
                                 connection.channel.failure().message(),
                      &sink);
      continue;
    }
    (void)connection.channel.pump_in();
    if (!connection.channel.failure().ok()) {
      drop_connection(index, "the connection failed while reading: " +
                                 connection.channel.failure().message(),
                      &sink);
      continue;
    }

    const Timestamp now = clock_->monotonic_now();
    std::vector<Frame> frames = connection.channel.take_inbound();
    if (!frames.empty()) {
      connection.last_activity = now;
    }
    for (const Frame& frame : frames) {
      stats_.bytes_in += static_cast<std::uint64_t>(frame.body.size()) + kFrameHeaderBytes;
      handle_frame(connection, frame, sink);
      if (connection.channel.closed() || !connection.channel.failure().ok()) {
        break;
      }
    }
    if (connection.channel.closed() || !connection.channel.failure().ok()) {
      const Status failure = connection.channel.failure();
      drop_connection(index,
                      failure.ok() ? std::string("the connection was closed after a protocol "
                                                 "violation")
                                   : "the connection failed while handling a frame: " +
                                         failure.message(),
                      &sink);
      continue;
    }

    // Silence is measured on the monotonic clock and driven by traffic: a
    // worker that stops heartbeating is dropped rather than held open forever.
    const Duration silent = now - connection.last_activity;
    if (silent > options_.silence_timeout) {
      const WorkerId worker = connection.worker;
      drop_connection(index,
                      "worker " + short_hex(worker) + " was silent for " +
                          std::to_string(silent.millis()) +
                          " ms, beyond the configured silence timeout of " +
                          std::to_string(options_.silence_timeout.millis()) + " ms",
                      &sink);
      continue;
    }
    ++index;
  }

  // A health request whose connection is gone can never be answered. Pruning it
  // here keeps the pending table bounded by the connections that can answer.
  if (pending_health_.size() > options_.limits.max_connections) {
    pending_health_.erase(pending_health_.begin(),
                          pending_health_.begin() +
                              static_cast<std::ptrdiff_t>(pending_health_.size() -
                                                          options_.limits.max_connections));
  }
}

// ---------------------------------------------------------------------------
// Frame handling
// ---------------------------------------------------------------------------

void ProcessAdapter::handle_frame(Connection& connection, const Frame& frame, IEvidenceSink& sink) {
  switch (frame.type) {
    case MessageType::kHello:
      handle_hello(connection, frame, sink);
      return;
    case MessageType::kDispatchAck:
      handle_dispatch_ack(connection, frame);
      return;
    case MessageType::kEvidence:
      handle_evidence(connection, frame, sink);
      return;
    case MessageType::kCancelAck:
      handle_cancel_ack(connection, frame);
      return;
    case MessageType::kReconcileResponse:
      handle_reconcile_response(connection, frame, sink);
      return;
    case MessageType::kHealthResponse:
      handle_health_response(connection, frame, sink);
      return;
    case MessageType::kHeartbeat: {
      auto decoded = decode_heartbeat(frame.body, options_.limits);
      if (!decoded.ok()) {
        connection.protocol_failure(stats_, &sink, clock_->wall_now(), next_sequence(), "heartbeat",
                                    decoded.status());
        return;
      }
      const ControllerIdentity identity = controller_identity(sink_hint_);
      HeartbeatAckMessage ack;
      ack.controller_incarnation = identity.incarnation;
      ack.controller_epoch = identity.epoch;
      ack.accept_work = true;
      (void)send_to(connection,
                    make_frame(MessageType::kHeartbeatAck, next_sequence(), encode(ack)));
      return;
    }
    case MessageType::kError: {
      auto decoded = decode_error(frame.body, options_.limits);
      if (!decoded.ok()) {
        connection.protocol_failure(stats_, &sink, clock_->wall_now(), next_sequence(), "error",
                                    decoded.status());
        return;
      }
      connection.protocol_failure(
          stats_, &sink, clock_->wall_now(), next_sequence(), "the worker reported an error",
          make_status(decoded.value().code, decoded.value().detail));
      return;
    }
    default:
      connection.protocol_failure(
          stats_, &sink, clock_->wall_now(), next_sequence(), "unexpected frame",
          make_status(StatusCode::kCorrupt, "a worker does not send " +
                                                std::string(to_string(frame.type)) +
                                                " to a controller"));
      return;
  }
}

void ProcessAdapter::handle_hello(Connection& connection, const Frame& frame, IEvidenceSink& sink) {
  auto decoded = decode_hello(frame.body, options_.limits);
  if (!decoded.ok()) {
    connection.protocol_failure(stats_, &sink, clock_->wall_now(), next_sequence(), "hello",
                                decoded.status());
    return;
  }
  const HelloMessage& message = decoded.value();
  if (message.abi_version != kRuntimeAbiVersion) {
    connection.protocol_failure(
        stats_, &sink, clock_->wall_now(), next_sequence(), "hello",
        make_status(StatusCode::kNotSupported,
                    "the worker announces ABI version " + std::to_string(message.abi_version) +
                        " and this controller speaks " + std::to_string(kRuntimeAbiVersion)));
    return;
  }
  if (message.worker.is_nil() || message.incarnation.is_nil()) {
    connection.protocol_failure(
        stats_, &sink, clock_->wall_now(), next_sequence(), "hello",
        make_status(StatusCode::kCorrupt, "the worker announced no identity"));
    return;
  }

  // A worker that announces itself again supersedes its older connection: two
  // live connections for one worker would double every retry.
  for (std::size_t other = 0; other < connections_.size(); ++other) {
    Connection& candidate = *connections_[other];
    if (candidate.index == connection.index) {
      continue;
    }
    if (!candidate.hello_received || !(candidate.worker == message.worker)) {
      continue;
    }
    const bool changed = !(candidate.incarnation == message.incarnation);
    candidate.channel.close();
    emit_event(&sink,
               changed ? AdapterEventKind::kWorkerIncarnationChanged
                       : AdapterEventKind::kWorkerDisconnected,
               candidate.worker, candidate.incarnation,
               changed ? "worker " + short_hex(candidate.worker) +
                             " reconnected under incarnation " +
                             candidate.incarnation.to_hex().substr(0, 12) +
                             "; the superseded connection is closed"
                       : "worker " + short_hex(candidate.worker) +
                             " announced itself again; the superseded connection is closed",
               clock_->wall_now());
  }

  connection.worker = message.worker;
  connection.incarnation = message.incarnation;
  connection.name = message.name;
  connection.capacity =
      std::min(message.capacity, options_.limits.max_concurrent_attempts);
  connection.in_flight = 0;
  connection.hello_received = true;
  connection.last_activity = clock_->monotonic_now();

  const ControllerIdentity identity = controller_identity(sink_hint_);
  HelloAckMessage ack;
  ack.protocol_version = kProtocolVersion;
  ack.abi_version = kRuntimeAbiVersion;
  ack.controller_incarnation = identity.incarnation;
  ack.controller_epoch = identity.epoch;
  ack.max_in_flight = connection.capacity;
  const Status sent =
      send_to(connection, make_frame(MessageType::kHelloAck, next_sequence(), encode(ack)));
  if (!sent.ok()) {
    connection.protocol_failure(stats_, &sink, clock_->wall_now(), next_sequence(), "hello ack",
                                sent);
    return;
  }

  emit_event(&sink, AdapterEventKind::kWorkerConnected, connection.worker, connection.incarnation,
             "worker " + short_hex(connection.worker) + " attached with capacity " +
                 std::to_string(connection.capacity),
             clock_->wall_now());
}

void ProcessAdapter::handle_dispatch_ack(Connection& connection, const Frame& frame) {
  auto decoded = decode_dispatch_ack(frame.body, options_.limits);
  if (!decoded.ok()) {
    connection.protocol_failure(stats_, sink_hint_, clock_->wall_now(), next_sequence(),
                                "dispatch ack", decoded.status());
    return;
  }
  const DispatchAckMessage& ack = decoded.value();
  const auto route = attempt_routes_.find(ack.fence.attempt);
  if (route == attempt_routes_.end() || route->second != connection.index) {
    return;  // an acknowledgement for an attempt this connection does not carry
  }
  if (ack.outcome == DispatchOutcome::kAccepted) {
    // Acknowledged. The route stays until the attempt publishes terminal
    // evidence or the worker refuses it; acceptance still proves nothing about
    // the effect.
    return;
  }
  if (connection.in_flight > 0) {
    --connection.in_flight;
  }
  attempt_routes_.erase(route);
  ++stats_.dispatch_refused;
  emit_event(sink_hint_, AdapterEventKind::kDispatchRefused, connection.worker,
             connection.incarnation,
             "worker " + short_hex(connection.worker) + " answered attempt " +
                 short_hex(ack.fence.attempt) + " with " +
                 std::string(to_string(ack.outcome)) + ": " + ack.detail,
             clock_->wall_now());
}

void ProcessAdapter::handle_evidence(Connection& connection, const Frame& frame,
                                     IEvidenceSink& sink) {
  auto decoded = decode_evidence(frame.body, options_.limits);
  if (!decoded.ok()) {
    ++stats_.evidence_rejected;
    connection.protocol_failure(stats_, &sink, clock_->wall_now(), next_sequence(), "evidence",
                                decoded.status());
    return;
  }
  EvidenceRecord record = std::move(decoded).value().record;

  // Adapter attestation: the adapter attests that this report came from the
  // executor it dispatched to. A worker may not vouch for a verifier or a
  // health source, so any wider authority it claims is narrowed to the
  // adapter's own, and a self-report is promoted to a live, attested one.
  if (record.authority == EvidenceAuthority::kWorkerSelfReported) {
    record.origin = EvidenceOrigin::kLive;
  }
  record.authority = EvidenceAuthority::kExecutionAdapterAttested;

  const auto route = attempt_routes_.find(record.attempt);
  if (route != attempt_routes_.end() && route->second == connection.index) {
    const bool terminal = record.kind == EvidenceKind::kExecutionCompleted ||
                          record.kind == EvidenceKind::kExecutionFailed ||
                          record.kind == EvidenceKind::kRollbackCompleted ||
                          record.kind == EvidenceKind::kRollbackFailed ||
                          record.kind == EvidenceKind::kCancellationAcknowledged;
    if (terminal) {
      attempt_routes_.erase(route);
      if (connection.in_flight > 0) {
        --connection.in_flight;
      }
      if (record.kind == EvidenceKind::kExecutionCompleted) {
        ++connection.completed;
      } else if (record.kind == EvidenceKind::kExecutionFailed) {
        ++connection.failed;
      }
    }
  }

  ++stats_.evidence_delivered;
  sink.on_evidence(std::move(record));
}

void ProcessAdapter::handle_cancel_ack(Connection& connection, const Frame& frame) {
  auto decoded = decode_cancel_ack(frame.body, options_.limits);
  if (!decoded.ok()) {
    connection.protocol_failure(stats_, sink_hint_, clock_->wall_now(), next_sequence(),
                                "cancel ack", decoded.status());
    return;
  }
  const CancelAckMessage& ack = decoded.value();
  const auto route = attempt_routes_.find(ack.fence.attempt);
  if (route == attempt_routes_.end() || route->second != connection.index) {
    return;
  }
  if (!ack.stopped) {
    return;
  }
  // The worker confirmed it stopped. The attempt stays routed: its
  // cancellation evidence, not this acknowledgement, is what the state machine
  // records.
  attempt_routes_.erase(route);
  if (connection.in_flight > 0) {
    --connection.in_flight;
  }
}

void ProcessAdapter::handle_reconcile_response(Connection& connection, const Frame& frame,
                                               IEvidenceSink& sink) {
  auto decoded = decode_reconcile_response(frame.body, options_.limits);
  if (!decoded.ok()) {
    connection.protocol_failure(stats_, &sink, clock_->wall_now(), next_sequence(),
                                "reconcile response", decoded.status());
    return;
  }
  const ReconcileResponseMessage& message = decoded.value();
  PendingReconcile* pending = nullptr;
  for (PendingReconcile& candidate : pending_reconciles_) {
    if (candidate.request_id == message.request_id) {
      pending = &candidate;
      break;
    }
  }
  if (pending == nullptr) {
    // Not a framing violation: the answer simply belongs to a request this
    // adapter no longer holds. It is recorded and the connection keeps serving.
    ++stats_.protocol_errors;
    emit_event(&sink, AdapterEventKind::kProtocolError, connection.worker, connection.incarnation,
               "worker " + short_hex(connection.worker) + " answered reconcile request " +
                   std::to_string(message.request_id) + ", which is not outstanding",
               clock_->wall_now());
    return;
  }
  pending->answered = true;
  pending->result.ok = true;
  pending->result.entries = message.entries;
  pending->result.detail = std::to_string(message.entries.size()) +
                           " entr(y/ies) answered by worker " + short_hex(connection.worker);

  for (const ReconcileEntry& entry : message.entries) {
    EvidenceKind kind = EvidenceKind::kExecutionProgress;
    EvidenceOutcome outcome = EvidenceOutcome::kInconclusive;
    switch (entry.disposition) {
      case ReconcileDisposition::kCompleted:
        kind = EvidenceKind::kExecutionCompleted;
        outcome = EvidenceOutcome::kSucceeded;
        break;
      case ReconcileDisposition::kFailed:
        kind = EvidenceKind::kExecutionFailed;
        outcome = EvidenceOutcome::kFailed;
        break;
      case ReconcileDisposition::kInProgress:
        kind = EvidenceKind::kExecutionProgress;
        outcome = EvidenceOutcome::kInconclusive;
        break;
      case ReconcileDisposition::kNotStarted:
      case ReconcileDisposition::kUnknown:
        // Nothing happened, or nothing can be proved: neither is a record about
        // the effect.
        continue;
    }
    const AttemptAnchor anchor = anchor_attempt(&sink, entry.fence.attempt);
    EvidenceRecord record;
    record.id = adapter_ids().next<EvidenceId>(IdDomain::kEvidence);
    record.rollout = entry.fence.rollout;
    record.generation = entry.fence.generation;
    record.stage = entry.fence.stage;
    record.target = anchor.known ? anchor.target : TargetId{};
    record.attempt = entry.fence.attempt;
    record.attempt_epoch = entry.fence.epoch;
    record.incarnation = entry.fence.incarnation;
    record.kind = kind;
    record.outcome = outcome;
    record.authority = EvidenceAuthority::kExecutionAdapterAttested;
    record.origin = EvidenceOrigin::kReconciledFromAdapter;
    record.sequence = anchor.known ? anchor.sequence : next_sequence();
    record.observed_at = clock_->wall_now();
    record.recorded_at = record.observed_at;
    record.payload_digest = sha256(std::string(to_string(entry.disposition)));
    ++stats_.evidence_delivered;
    sink.on_evidence(std::move(record));
  }
}

void ProcessAdapter::handle_health_response(Connection& connection, const Frame& frame,
                                            IEvidenceSink& sink) {
  auto decoded = decode_health_response(frame.body, options_.limits);
  if (!decoded.ok()) {
    connection.protocol_failure(stats_, &sink, clock_->wall_now(), next_sequence(),
                                "health response", decoded.status());
    return;
  }
  const HealthResponseMessage& message = decoded.value();
  for (std::size_t index = pending_health_.size(); index > 0; --index) {
    if (pending_health_[index - 1].request_id == message.request_id) {
      pending_health_.erase(pending_health_.begin() + static_cast<std::ptrdiff_t>(index - 1));
      break;
    }
  }

  // The adapter's configured source is what a gate names. A worker's own source
  // identifier is only used when the adapter has none, so an unattributed
  // sample can never masquerade as telemetry from the configured source.
  const HealthSourceId source =
      health_source_.is_nil() ? message.source : health_source_;

  for (const HealthSample& sample : message.samples) {
    const auto found = std::lower_bound(
        latest_health_.begin(), latest_health_.end(), sample.target,
        [](const HealthSample& held, const TargetId& probe) { return held.target < probe; });
    if (found != latest_health_.end() && found->target == sample.target) {
      *found = sample;
    } else {
      latest_health_.insert(found, sample);
    }
  }
  latest_health_at_ = message.observed_at;
  latest_health_live_ = message.live;

  const ControllerIdentity identity = controller_identity(sink_hint_);
  for (const HealthSample& sample : message.samples) {
    EvidenceRecord record;
    record.id = adapter_ids().next<EvidenceId>(IdDomain::kEvidence);
    record.target = sample.target;
    record.kind = EvidenceKind::kHealthSample;
    record.outcome =
        sample.healthy ? EvidenceOutcome::kSucceeded : EvidenceOutcome::kFailed;
    record.authority = EvidenceAuthority::kHealthSourceAttested;
    record.origin = EvidenceOrigin::kLive;
    record.sequence = next_sequence();
    record.observed_at = sample.observed_at;
    record.recorded_at = clock_->wall_now();
    record.incarnation = identity.incarnation;
    record.health_source = source;
    record.payload_digest = sha256(std::string(sample.healthy ? "healthy" : "unhealthy"));
    ++stats_.evidence_delivered;
    sink.on_evidence(std::move(record));
  }
}

// ---------------------------------------------------------------------------
// Reconciliation
// ---------------------------------------------------------------------------

ReconcileResult ProcessAdapter::reconcile(const std::vector<AttemptFence>& attempts) {
  ++stats_.reconcile_requests;
  ReconcileResult result;
  if (attempts.empty()) {
    result.ok = true;
    result.detail = "no attempts were submitted for reconciliation";
    return result;
  }
  if (attempts.size() > options_.limits.max_concurrent_attempts) {
    result.detail = "the reconciliation names " + std::to_string(attempts.size()) +
                    " attempts, above max_concurrent_attempts";
    return result;
  }

  std::map<AttemptId, ReconcileEntry> answers;
  std::map<std::size_t, std::vector<AttemptFence>> groups;
  std::size_t unique = 0;
  for (const AttemptFence& fence : attempts) {
    if (answers.find(fence.attempt) != answers.end()) {
      continue;  // a repeated fence is answered once
    }
    ++unique;
    const auto route = attempt_routes_.find(fence.attempt);
    if (route == attempt_routes_.end() || route->second >= connections_.size()) {
      ReconcileEntry entry;
      entry.fence = fence;
      entry.disposition = ReconcileDisposition::kUnknown;
      entry.outcome = EvidenceOutcome::kInconclusive;
      entry.detail = "the connection that carried this attempt is gone; losing contact is not "
                     "evidence that the effect did not happen";
      answers.emplace(fence.attempt, std::move(entry));
      continue;
    }
    groups[route->second].push_back(fence);
  }

  // Groups are visited from the highest connection index down, so dropping a
  // dead connection can never renumber a group that has not been sent yet.
  std::vector<std::uint64_t> request_ids;
  for (auto group = groups.rbegin(); group != groups.rend(); ++group) {
    const std::size_t index = group->first;
    if (index >= connections_.size()) {
      continue;
    }
    Connection& connection = *connections_[index];
    PendingReconcile pending;
    pending.request_id = ++next_request_id_;
    pending.connection_index = index;
    pending.fences = group->second;
    ReconcileRequestMessage message;
    message.request_id = pending.request_id;
    message.fences = pending.fences;

    const Status sent = send_to(connection,
                                make_frame(MessageType::kReconcileRequest, next_sequence(),
                                           encode(message)));
    if (!sent.ok()) {
      for (const AttemptFence& fence : pending.fences) {
        ReconcileEntry entry;
        entry.fence = fence;
        entry.disposition = ReconcileDisposition::kUnknown;
        entry.outcome = EvidenceOutcome::kInconclusive;
        entry.detail = "the reconcile request could not be delivered: " + sent.message();
        answers[fence.attempt] = std::move(entry);
      }
      drop_connection(index, "the reconcile request could not be delivered: " + sent.message(),
                      sink_hint_);
      continue;
    }
    request_ids.push_back(pending.request_id);
    pending_reconciles_.push_back(std::move(pending));
  }

  // Bounded block-poll: the adapter owns this loop because the orchestrator
  // calls reconcile() outside its tick. Every wait is a slice of the connect
  // timeout and the whole loop is bounded by one connect timeout.
  const std::int64_t budget_nanos =
      options_.connect_timeout.nanos() > 0 ? options_.connect_timeout.nanos() : 0;
  Duration slice = options_.connect_timeout;
  if (slice > Duration::from_millis(kReconcileSliceMillis)) {
    slice = Duration::from_millis(kReconcileSliceMillis);
  }
  if (slice <= Duration::from_nanos(0)) {
    slice = Duration::from_millis(1);
  }
  const Timestamp started = clock_->monotonic_now();
  if (sink_hint_ != nullptr && !request_ids.empty()) {
    for (;;) {
      std::size_t outstanding = 0;
      for (const PendingReconcile& pending : pending_reconciles_) {
        if (!pending.answered && mentions(request_ids, pending.request_id)) {
          ++outstanding;
        }
      }
      if (outstanding == 0) {
        break;
      }
      const std::int64_t elapsed = (clock_->monotonic_now() - started).nanos();
      if (elapsed >= budget_nanos) {
        break;
      }
      Duration wait = Duration::from_nanos(budget_nanos - elapsed);
      if (slice < wait) {
        wait = slice;
      }
      for (std::size_t index = 0; index < connections_.size();) {
        Connection& connection = *connections_[index];
        if (connection.channel.closed()) {
          drop_connection(index, "the connection was closed while a reconcile answer was "
                                 "outstanding",
                          sink_hint_);
          continue;
        }
        (void)connection.channel.pump_out();
        if (!connection.channel.failure().ok()) {
          drop_connection(index, "the connection failed while a reconcile answer was "
                                 "outstanding: " + connection.channel.failure().message(),
                          sink_hint_);
          continue;
        }
        SelectResult selected{};
        const Status waited = select_one(connection.channel.socket(), false, wait, selected);
        if (!waited.ok()) {
          drop_connection(index,
                          "waiting for a reconcile answer failed: " + waited.message(),
                          sink_hint_);
          continue;
        }
        if (selected.failed) {
          drop_connection(index,
                          "the socket reported an error while a reconcile answer was outstanding",
                          sink_hint_);
          continue;
        }
        if (selected.readable) {
          (void)connection.channel.pump_in();
          if (!connection.channel.failure().ok()) {
            drop_connection(index,
                            "the connection failed while reading a reconcile answer: " +
                                connection.channel.failure().message(),
                            sink_hint_);
            continue;
          }
          std::vector<Frame> frames = connection.channel.take_inbound();
          for (const Frame& frame : frames) {
            stats_.bytes_in += static_cast<std::uint64_t>(frame.body.size()) + kFrameHeaderBytes;
            handle_frame(connection, frame, *sink_hint_);
            if (connection.channel.closed() || !connection.channel.failure().ok()) {
              break;
            }
          }
          if (connection.channel.closed() || !connection.channel.failure().ok()) {
            drop_connection(index,
                            "the connection failed while handling a frame received during "
                            "reconciliation",
                            sink_hint_);
            continue;
          }
        }
        ++index;
      }
    }
  }

  std::size_t unanswered_requests = 0;
  for (const std::uint64_t id : request_ids) {
    for (const PendingReconcile& pending : pending_reconciles_) {
      if (pending.request_id != id) {
        continue;
      }
      if (!pending.answered) {
        ++unanswered_requests;
        for (const AttemptFence& fence : pending.fences) {
          ReconcileEntry entry;
          entry.fence = fence;
          entry.disposition = ReconcileDisposition::kUnknown;
          entry.outcome = EvidenceOutcome::kInconclusive;
          entry.detail = "no answer arrived within the reconcile budget";
          answers[fence.attempt] = std::move(entry);
        }
      } else {
        for (const ReconcileEntry& entry : pending.result.entries) {
          answers[entry.fence.attempt] = entry;
        }
      }
      break;
    }
  }
  pending_reconciles_.erase(
      std::remove_if(pending_reconciles_.begin(), pending_reconciles_.end(),
                     [&request_ids](const PendingReconcile& pending) {
                       return mentions(request_ids, pending.request_id);
                     }),
      pending_reconciles_.end());

  std::set<AttemptId> emitted;
  std::size_t unknown = 0;
  for (const AttemptFence& fence : attempts) {
    if (!emitted.insert(fence.attempt).second) {
      continue;
    }
    const auto found = answers.find(fence.attempt);
    if (found == answers.end()) {
      continue;
    }
    if (found->second.disposition == ReconcileDisposition::kUnknown) {
      ++unknown;
    }
    result.entries.push_back(found->second);
  }

  result.ok = unanswered_requests == 0 && unknown == 0 && result.entries.size() == unique;
  if (result.ok) {
    result.detail = "reconciled " + std::to_string(result.entries.size()) +
                    " attempt(s) against live executor(s)";
  } else {
    result.detail = std::to_string(unanswered_requests) +
                    " reconcile request(s) went unanswered and " + std::to_string(unknown) +
                    " attempt(s) have no live owner; their fate is unknown and no outcome was "
                    "assumed";
  }
  return result;
}

// ---------------------------------------------------------------------------
// IHealthAdapter
// ---------------------------------------------------------------------------

Status ProcessAdapter::request_probe(const HealthProbeRequest& request) {
  if (request.targets.size() > options_.limits.max_total_targets) {
    return make_status(StatusCode::kLimitExceeded,
                       "the probe names " + std::to_string(request.targets.size()) +
                           " targets, above max_total_targets");
  }
  if (pending_health_.size() >= options_.limits.max_connections) {
    return make_status(StatusCode::kResourceExhausted,
                       "the adapter already holds the maximum number of unanswered probes");
  }
  if (!available()) {
    return make_status(StatusCode::kUnavailable,
                       "no attached worker can answer a health probe");
  }

  const std::uint64_t request_id =
      request.request_id != 0 ? request.request_id : ++next_request_id_;
  HealthRequestMessage message;
  message.request_id = request_id;
  message.targets = request.targets;
  message.issued_at = clock_->wall_now();

  PendingHealth pending;
  pending.request_id = request_id;
  pending.targets = request.targets;
  pending.issued_at = message.issued_at;
  pending_health_.push_back(std::move(pending));

  std::size_t sent = 0;
  Status last = Status::success();
  for (auto& connection : connections_) {
    if (!connection->usable()) {
      continue;
    }
    const Status status =
        send_to(*connection,
                make_frame(MessageType::kHealthRequest, next_sequence(), encode(message)));
    if (status.ok()) {
      ++sent;
    } else {
      last = status;
    }
  }
  if (sent == 0) {
    pending_health_.pop_back();
    return last;
  }
  return Status::success();
}

HealthSnapshotView ProcessAdapter::latest(const std::vector<TargetId>& targets) const {
  HealthSnapshotView view;
  view.source = health_source_;
  view.observed_at = latest_health_at_;
  view.live = latest_health_live_;
  if (targets.empty()) {
    view.samples = latest_health_;
  } else {
    for (const TargetId& target : targets) {
      const auto found = std::lower_bound(
          latest_health_.begin(), latest_health_.end(), target,
          [](const HealthSample& held, const TargetId& probe) { return held.target < probe; });
      if (found != latest_health_.end() && found->target == target) {
        view.samples.push_back(*found);
      }
    }
  }
  view.available = !view.samples.empty();
  return view;
}

// ---------------------------------------------------------------------------
// Supervision
// ---------------------------------------------------------------------------

std::vector<WorkerId> ProcessAdapter::attached_workers() const {
  std::vector<WorkerId> workers;
  for (const auto& connection : connections_) {
    if (!connection->usable()) {
      continue;
    }
    workers.push_back(connection->worker);
  }
  return workers;
}

std::uint32_t ProcessAdapter::in_flight_total() const noexcept {
  std::uint32_t total = 0;
  for (const auto& connection : connections_) {
    total += connection->in_flight;
  }
  return total;
}

Status ProcessAdapter::request_shutdown(std::string reason) {
  ShutdownMessage message;
  message.reason = std::move(reason);
  const std::vector<std::byte> body = encode(message);

  std::size_t sent = 0;
  Status last = Status::success();
  for (auto& connection : connections_) {
    if (!connection->usable()) {
      continue;
    }
    const Status status = send_to(*connection,
                                  make_frame(MessageType::kShutdown, next_sequence(),
                                             std::vector<std::byte>(body)));
    if (status.ok()) {
      ++sent;
    } else {
      last = status;
    }
  }
  // Bounded best effort: a worker that does not read its shutdown frame in time
  // is closed anyway by close_all().
  for (auto& connection : connections_) {
    if (connection->channel.closed() || !connection->channel.failure().ok()) {
      continue;
    }
    (void)connection->channel.flush_out(Duration::from_millis(kShutdownFlushMillis));
  }
  if (sent == 0 && !connections_.empty()) {
    return last;
  }
  return Status::success();
}

void ProcessAdapter::close_all() {
  for (std::size_t position = connections_.size(); position > 0; --position) {
    Connection& connection = *connections_[position - 1];
    if (connection.hello_received) {
      ++stats_.reconnects;
      emit_event(nullptr, AdapterEventKind::kWorkerDisconnected, connection.worker,
                 connection.incarnation, "the controller closed the connection",
                 clock_->wall_now());
    }
    connection.channel.close();
  }
  connections_.clear();
  attempt_routes_.clear();
  pending_reconciles_.clear();
  pending_health_.clear();
}

// ---------------------------------------------------------------------------
// Connection lifetime
// ---------------------------------------------------------------------------

void ProcessAdapter::drop_connection(std::size_t index, std::string detail, IEvidenceSink* sink) {
  if (index >= connections_.size()) {
    return;
  }
  Connection& connection = *connections_[index];
  const WorkerId worker = connection.worker;
  const IncarnationId incarnation = connection.incarnation;
  connection.channel.close();
  ++stats_.reconnects;

  // A worker that reconnected under the same incarnation still owns the effects
  // it was carrying, so its routes are re-pointed at the live connection. When
  // no such connection exists the routes are forgotten: the attempts become
  // unknown, which is the honest answer, and reconcile() reports exactly that.
  std::size_t replacement = connections_.size();
  for (std::size_t other = 0; other < connections_.size(); ++other) {
    if (other == index) {
      continue;
    }
    Connection& candidate = *connections_[other];
    if (!candidate.usable()) {
      continue;
    }
    if (!(candidate.worker == worker) || !(candidate.incarnation == incarnation)) {
      continue;
    }
    replacement = other;
    break;
  }
  const bool reconnected = replacement != connections_.size();
  const std::size_t rekeyed = replacement > index ? replacement - 1 : replacement;

  std::vector<AttemptId> owned;
  for (const auto& entry : attempt_routes_) {
    if (entry.second == index) {
      owned.push_back(entry.first);
    }
  }
  std::sort(owned.begin(), owned.end());
  for (const AttemptId& attempt : owned) {
    attempt_routes_.erase(attempt);
  }
  for (auto& entry : attempt_routes_) {
    if (entry.second > index) {
      --entry.second;
    }
  }
  if (reconnected) {
    for (const AttemptId& attempt : owned) {
      attempt_routes_[attempt] = rekeyed;
    }
  }

  for (PendingReconcile& pending : pending_reconciles_) {
    if (pending.connection_index == index) {
      if (!pending.answered) {
        pending.answered = true;
        pending.result.ok = false;
        pending.result.detail = "the worker connection dropped before it answered";
        for (const AttemptFence& fence : pending.fences) {
          ReconcileEntry entry;
          entry.fence = fence;
          entry.disposition = ReconcileDisposition::kUnknown;
          entry.outcome = EvidenceOutcome::kInconclusive;
          entry.detail = "the connection dropped before the executor answered the reconcile "
                         "request";
          pending.result.entries.push_back(std::move(entry));
        }
      }
    } else if (pending.connection_index > index) {
      --pending.connection_index;
    }
  }

  connections_.erase(connections_.begin() + static_cast<std::ptrdiff_t>(index));
  for (std::size_t position = index; position < connections_.size(); ++position) {
    connections_[position]->index = position;
  }
  emit_event(sink, AdapterEventKind::kWorkerDisconnected, worker, incarnation, std::move(detail),
             clock_->wall_now());
}

}  // namespace rollout_fabric
