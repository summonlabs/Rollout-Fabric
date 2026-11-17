// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A worker served in process by a minimal controller over real loopback
// sockets. A dispatch that arrives in the same read as the hello
// acknowledgement is served, and finished effect threads are released while
// the worker keeps serving, so a long-lived worker does not hold one thread for
// every attempt it has ever served.
//
// Every wait below is bounded by a round count, so a worker that stops
// answering fails the test instead of hanging the suite.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "rollout_fabric/digest.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/protocol.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"
#include "rollout_fabric/transport.hpp"
#include "rollout_fabric/version.hpp"
#include "rollout_fabric/worker.hpp"
#include "test_harness.hpp"

using namespace rollout_fabric;

namespace {

constexpr int kWaitRounds = 2000;  // at 5 ms per round, about 10 s

[[nodiscard]] TargetId target_named(const std::string& name) {
  const Digest256 digest = sha256(name);
  return TargetId::from_bytes(
      std::span<const std::byte, TargetId::byte_size>(digest.data(), TargetId::byte_size));
}

// A controller always sends a complete fence. The worker copies it into its
// evidence, and evidence without a rollout, generation and stage does not
// validate.
struct ControllerFence {
  RolloutId rollout{};
  GenerationId generation{};
  StageId stage{};
  IncarnationId incarnation{};
};

[[nodiscard]] ControllerFence make_fence(IdFactory& ids) {
  ControllerFence fence;
  fence.rollout = ids.next<RolloutId>(IdDomain::kRollout);
  fence.generation = ids.next<GenerationId>(IdDomain::kGeneration);
  fence.stage = ids.next<StageId>(IdDomain::kStage);
  fence.incarnation = ids.next<IncarnationId>(IdDomain::kIncarnation);
  return fence;
}

[[nodiscard]] HelloAckMessage make_hello_ack(const ControllerFence& fence) {
  HelloAckMessage ack;
  ack.abi_version = kRuntimeAbiVersion;
  ack.controller_incarnation = fence.incarnation;
  ack.controller_epoch = EpochCounter{1};
  ack.max_in_flight = 4;
  return ack;
}

[[nodiscard]] DispatchCommandMessage make_dispatch(IdFactory& ids, const ControllerFence& fence,
                                                   const std::string& name) {
  DispatchCommandMessage dispatch;
  dispatch.request.fence.rollout = fence.rollout;
  dispatch.request.fence.generation = fence.generation;
  dispatch.request.fence.stage = fence.stage;
  dispatch.request.fence.attempt = ids.next<AttemptId>(IdDomain::kAttempt);
  dispatch.request.fence.incarnation = fence.incarnation;
  dispatch.target = target_named(name);
  dispatch.request.target = dispatch.target;
  dispatch.target_name = name;
  dispatch.request.idempotency_key = sha256(name);
  return dispatch;
}

[[nodiscard]] bool is_completion_of(const Frame& frame, const AttemptId& attempt,
                                    const RuntimeLimits& limits) {
  if (frame.type != MessageType::kEvidence) {
    return false;
  }
  auto evidence = decode_evidence(frame.body, limits);
  return evidence.ok() && evidence.value().record.attempt == attempt &&
         evidence.value().record.kind == EvidenceKind::kExecutionCompleted;
}

[[nodiscard]] std::optional<Socket> accept_connection(Socket& listener) {
  for (int round = 0; round < kWaitRounds; ++round) {
    SelectResult selected{};
    if (!select_one(listener, false, Duration::from_millis(5), selected).ok()) {
      return std::nullopt;
    }
    if (selected.readable) {
      auto accepted = listener.accept();
      if (!accepted.ok()) {
        return std::nullopt;
      }
      return std::move(accepted).value();
    }
  }
  return std::nullopt;
}

// Pumps the channel until `match` accepts an inbound frame.
template <class Match>
[[nodiscard]] bool await_frame(FrameChannel& channel, Match match) {
  for (int round = 0; round < kWaitRounds; ++round) {
    (void)channel.pump_out();
    (void)channel.pump_in();
    if (!channel.failure().ok()) {
      return false;
    }
    for (const Frame& frame : channel.take_inbound()) {
      if (match(frame)) {
        return true;
      }
    }
    SelectResult selected{};
    if (!select_one(channel.socket(), channel.outbound_backlog() > 0, Duration::from_millis(5),
                    selected)
             .ok()) {
      return false;
    }
  }
  return false;
}

// Reads until the worker closes its end. Closing this end with the worker's
// frames still unread would reset the connection, and a reset can discard the
// shutdown request before the worker has read it.
void await_worker_close(FrameChannel& channel) {
  for (int round = 0; round < kWaitRounds; ++round) {
    (void)channel.pump_out();
    (void)channel.pump_in();
    (void)channel.take_inbound();
    if (!channel.failure().ok()) {
      return;
    }
    SelectResult selected{};
    if (!select_one(channel.socket(), channel.outbound_backlog() > 0, Duration::from_millis(5),
                    selected)
             .ok()) {
      return;
    }
  }
}

void shut_down_worker(FrameChannel& channel, std::uint64_t sequence) {
  ShutdownMessage shutdown;
  shutdown.reason = "the test is finished";
  (void)channel.queue(make_frame(MessageType::kShutdown, Sequence{sequence}, encode(shutdown)));
  (void)channel.flush_out(Duration::from_seconds(5));
  await_worker_close(channel);
}

}  // namespace

RF_TEST(a_dispatch_that_arrives_with_the_hello_ack_is_served) {
  RF_REQUIRE(ensure_socket_runtime().ok());
  auto listening = Socket::listen_loopback(0);
  RF_REQUIRE(listening.ok());
  Socket listener = std::move(listening).value();
  const RuntimeLimits limits{};

  WorkerOptions options;
  options.controller_port = listener.bound_port();
  options.name = "pipelined-dispatch";
  options.scenario.fallback.work_duration = Duration::from_millis(0);
  auto created = Worker::create(options);
  RF_REQUIRE(created.ok());
  std::unique_ptr<Worker> worker = std::move(created).value();

  Status ran = Status::success();
  std::thread runner([&worker, &ran] { ran = worker->run(); });

  // Nothing below returns early: the outcome is recorded, the worker is shut
  // down and joined, and only then is anything checked.
  IdFactory ids = IdFactory::from_entropy();
  const ControllerFence fence = make_fence(ids);
  bool sent = false;
  bool completed = false;
  FrameChannel channel;
  std::optional<Socket> connection = accept_connection(listener);
  if (connection.has_value()) {
    // A controller with work waiting may send the first dispatch right behind
    // its acknowledgement. Both leave here in one write, so the worker reads
    // them together, before it has left its handshake.
    const DispatchCommandMessage dispatch = make_dispatch(ids, fence, "pipelined");
    std::vector<std::byte> bytes;
    std::vector<std::byte> second;
    sent = encode_frame(make_frame(MessageType::kHelloAck, Sequence{1}, encode(make_hello_ack(fence))),
                        limits, bytes)
               .ok() &&
           encode_frame(make_frame(MessageType::kDispatch, Sequence{2}, encode(dispatch, limits)),
                        limits, second)
               .ok();
    if (sent) {
      bytes.insert(bytes.end(), second.begin(), second.end());
      std::size_t offset = 0;
      while (sent && offset < bytes.size()) {
        auto written = connection->send(std::span<const std::byte>(bytes).subspan(offset));
        sent = written.ok() && written.value() > 0;
        if (sent) {
          offset += written.value();
        }
      }
    }
    channel = FrameChannel(std::move(*connection), limits);
    const AttemptId attempt = dispatch.request.fence.attempt;
    completed = sent && await_frame(channel, [&attempt, &limits](const Frame& frame) {
                  return is_completion_of(frame, attempt, limits);
                });
    shut_down_worker(channel, 3);
  }
  channel.close();
  (void)listener.close();
  runner.join();

  RF_CHECK(connection.has_value());
  RF_CHECK(sent);
  // The dispatch that shared the acknowledgement's read was refused as
  // traffic from before the acknowledgement, and the worker ended its run.
  RF_CHECK(completed);
  RF_CHECK(ran.ok());
}

RF_TEST(finished_effect_threads_are_released_while_the_worker_serves) {
#if defined(_WIN32)
  RF_REQUIRE(ensure_socket_runtime().ok());
  auto listening = Socket::listen_loopback(0);
  RF_REQUIRE(listening.ok());
  Socket listener = std::move(listening).value();
  const RuntimeLimits limits{};

  WorkerOptions options;
  options.controller_port = listener.bound_port();
  options.name = "thread-release";
  options.scenario.fallback.work_duration = Duration::from_millis(0);
  auto created = Worker::create(options);
  RF_REQUIRE(created.ok());
  std::unique_ptr<Worker> worker = std::move(created).value();

  bool served = false;
  std::thread runner([&worker, &served] { served = worker->run().ok(); });

  // Nothing below returns early: the outcome is recorded, the worker is shut
  // down and joined, and only then is anything checked.
  constexpr std::uint32_t kAttempts = 100;
  IdFactory ids = IdFactory::from_entropy();
  const ControllerFence fence = make_fence(ids);
  std::uint64_t sequence = 0;
  bool handshake = false;
  std::uint32_t completed = 0;
  DWORD before = 0;
  DWORD after = 0;
  FrameChannel channel;
  std::optional<Socket> connection = accept_connection(listener);
  if (connection.has_value()) {
    channel = FrameChannel(std::move(*connection), limits);
    // The worker sends heartbeats only once it has accepted the
    // acknowledgement, so no dispatch below shares a read with it.
    handshake =
        await_frame(channel, [](const Frame& frame) { return frame.type == MessageType::kHello; }) &&
        channel
            .queue(make_frame(MessageType::kHelloAck, Sequence{++sequence},
                              encode(make_hello_ack(fence))))
            .ok() &&
        await_frame(channel,
                    [](const Frame& frame) { return frame.type == MessageType::kHeartbeat; });
  }
  if (handshake) {
    ::GetProcessHandleCount(::GetCurrentProcess(), &before);
    for (std::uint32_t index = 0; index < kAttempts; ++index) {
      // One attempt at a time, each on its own effect thread, each waited for
      // until its completion evidence arrives.
      const DispatchCommandMessage dispatch =
          make_dispatch(ids, fence, "target-" + std::to_string(index));
      const AttemptId attempt = dispatch.request.fence.attempt;
      if (!channel
               .queue(make_frame(MessageType::kDispatch, Sequence{++sequence},
                                 encode(dispatch, limits)))
               .ok()) {
        break;
      }
      const bool finished = await_frame(channel, [&attempt, &limits](const Frame& frame) {
        return is_completion_of(frame, attempt, limits);
      });
      if (!finished) {
        break;
      }
      ++completed;
    }
    ::GetProcessHandleCount(::GetCurrentProcess(), &after);
  }
  if (connection.has_value()) {
    shut_down_worker(channel, ++sequence);
  }
  channel.close();
  (void)listener.close();
  runner.join();

  RF_CHECK(handshake);
  RF_CHECK_EQ(completed, kAttempts);
  RF_CHECK(served);
  // A finished effect thread that is never joined keeps its handle, so the
  // count would grow by about one per attempt.
  RF_CHECK(after < before + 64);
#endif
}

RF_TEST_MAIN()
