# Rollout Fabric

Vendor-neutral rollout orchestration runtime for the Summon Software Labs Fabric OS.

Rollout Fabric owns **execution orchestration** across cohorts and stages. It consumes an
approved Change Planner plan plus deployable artifacts/actions, and it coordinates *when*
and *where* approved changes advance, pause, abort or roll back under explicit gates and
evidence. It does not invent target intent, does not render or distribute configuration,
and does not define maintenance eligibility. Those belong to the systems upstream of it.

    Change Planner plan  ->  Rollout Fabric  ->  execution adapters  ->  targets
      (approved intent)      (when / where)      (real processes)       (switches, devices)

## What is implemented

* **Typed identities.** `RolloutId`, `StageId`, `CohortId`, `TargetId`, `AttemptId`,
  `GenerationId`, `PlanId`, `ActionId`, `EvidenceId`, `DecisionId`, `IncarnationId`,
  `AuthorityId`, `WorkerId`, `FailureDomainId` and their siblings are distinct C++ types
  over a fixed-width value. `Revision`, `Epoch`, `AttemptEpoch` and `Sequence` are distinct
  counter types. A revision cannot be passed where an epoch is expected.
* **Cohort selection.** Selectors over device/switch identity, kind, site, pod, rack, failure
  domain, labels and set operations, evaluated against a versioned target inventory.
  Materialising a cohort freezes the member list, the selector digest, the inventory digest
  and the inventory epoch together. **Membership of an armed stage is immutable**; the only
  way to obtain different membership is `regenerate`, which mints a new `GenerationId` and
  re-materialises every cohort.
* **Stage policy.** Maximum concurrency, minimum healthy fraction, failure budget (count and
  fraction), canary size, soak duration, evidence gate, health gate, spread and blast-radius
  limits, per-attempt deadline, attempts per target, evidence lifetime, and explicit
  manual-versus-automatic gates for stage entry, evidence and advance.
* **Lifecycle.** `created` to `validated` to `armed` to `running stage` to `gated`/`soaking`
  to `advancing`, plus `paused`, `aborting`, `rolling-back`, `failed`, `completed`, `retired`.
  Transitions are a closed table; an illegal transition is a typed conflict, never a silent
  state write.
* **Exactly-defined evidence.** Dispatch is not completion. An accepted dispatch proves only
  acceptance. Only terminal execution evidence from an attesting authority completes a
  target, and each stage declares the authority that may attest success and failure.
* **Fencing.** Stale rollout generation, stale attempt epoch, stale controller incarnation,
  stale revision, replayed evidence sequences and unknown attempts are each refused with a
  distinct status. A cancelled attempt can never later publish a success.
* **Pause.** New work stops immediately. Attempts already dispatched follow the stage's
  declared in-flight policy: `let_finish` (their terminal evidence is still applied) or
  `cancel` (they are told to stop and can never be counted as success).
* **Abort and compensation.** Abort stops future stages and starts compensation **only where
  the plan declares it**. A failed stage does the same. Compensation failures are recorded as
  `rollback_failed`, never hidden, and a rollout is never reported as finished while
  compensation is still outstanding.
* **Durable, integrity-checked state.** A versioned append-only journal with CRC-32 framing,
  SHA-256 payload digests, a checksummed header, an operating-system-enforced exclusive lock
  and conservative recovery: everything provable is kept, everything after the last provable
  record is discarded and reported. A tick that changed nothing does not write.
* **Restart-safe continuation.** A new controller incarnation adopts the journal, records an
  incarnation claim, and asks the execution adapter about every attempt that was outstanding
  before dispatching anything. Attempts the executor already completed are completed from its
  recorded outcome; attempts it never started are released for dispatch **under the same
  idempotency key**, so an effect that already happened cannot be repeated.
* **Freshness.** Every piece of evidence carries the time the underlying fact was observed,
  the authority that attested it and whether it was read live or restored. Stale telemetry
  cannot satisfy a gate, and a clock that moved backwards is treated as an anomaly.
* **Blast radius.** Global, stage, failure-domain, rack, pod and site limits on how much of a
  correlated population may be changing at once, enforced at the only place that can create
  work. The high-water mark is recorded per stage and per rollout as evidence that the limit
  was never exceeded.
* **Deterministic decisions.** Every transition is the consequence of exactly one recorded
  decision naming its inputs (by digest), the governing policy, the evidence weighed, the
  action selected, the alternatives rejected and why, and the generation, revision and
  controller incarnation under which it had the authority to act.
* **CLI.** `rollout-cli` with `create`, `arm`, `start`, `pause`, `resume`, `abort`, `approve`,
  `retire`, `regenerate`, `status`, `explain`, `stages`, `cohort` and `events`.
* **Inspection.** `rf-inspect` replays a journal offline (read-only, without taking the writer
  lock) and parses plan and inventory documents.

## Architecture

    include/rollout_fabric/   the installed contract
    src/                      the runtime
    tools/                    rolloutd, rf-worker, rollout-cli, rf-inspect
    examples/                 worked examples and the plan/inventory documents they use
    benchmarks/               completed-work measurements
    tests/                    unit, adversarial, property and real-process suites

The controller (`rolloutd`) is **single-threaded by construction**: the reactor, the
orchestrator, the adapters and the journal all run on one thread, so the state machine
contains no mutex, takes no lock and cannot deadlock or be re-entered. Adapters deliver
evidence by calling back into the orchestrator, and that evidence is queued and applied after
the adapter returns, so a callback can never re-enter the state machine. Concurrency lives in
separate operating system processes, which is where it is proved.

Workers (`rf-worker`) own real threads: one per active attempt, plus a mutex-guarded
completion queue. The mutex is held only long enough to move a value into the queue, never
across a socket call, a callback or an allocation that can fail.

## Build

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure

Requirements: CMake 3.20+, a C++20 compiler. Windows x64 with MSVC is the validated
configuration (`/W4 /WX`, zero first-party warnings). Options: `RF_BUILD_TESTS`,
`RF_BUILD_TOOLS`, `RF_BUILD_EXAMPLES`, `RF_BUILD_BENCHMARKS`, `RF_WARNINGS_AS_ERRORS`,
`RF_SANITIZERS`, `RF_ANALYZE`.

## Use

    # 1. start the controller with an approved plan and a target inventory
    rolloutd --journal state/rollout.journal \
             --plan examples/fabric.plan.json \
             --inventory examples/fabric.inventory.json \
             --auto-arm --auto-start
    rolloutd ready incarnation=<hex> epoch=1 control_port=<n> worker_port=<n> pid=<n>

    # 2. start workers that execute the attempts
    rf-worker --controller-port <worker_port> --name w1 --capacity 4

    # 3. observe and steer
    rollout-cli --port <control_port> status
    rollout-cli --port <control_port> status --rollout <hex>
    rollout-cli --port <control_port> explain --rollout <hex>
    rollout-cli --port <control_port> approve --rollout <hex> --gate <hex>
    rollout-cli --port <control_port> abort --rollout <hex> --reason "change window closed"

Library use starts at `rollout_fabric::Orchestrator`; `examples/single_stage_rollout.cpp` and
`examples/manual_gate_and_rollback.cpp` are complete, runnable programs.

## Plan and inventory documents

`examples/fabric.plan.json` and `examples/fabric.inventory.json` are the documents the tests
and examples use. A plan carries the upstream `source_digest`, the approval record, and stages
with a selector, a policy, an evidence rule and actions. `rf-inspect plan` and
`rf-inspect inventory` validate them and print the identifiers the runtime derives.

## Install and consume

    cmake --install build --prefix /some/prefix

    find_package(RolloutFabric 1.0 REQUIRED)
    target_link_libraries(consumer PRIVATE SummonSoftwareLabs::rollout_fabric)

## Verified capabilities

Every figure below was produced by running the artifacts in this repository.

| Suite | What it establishes |
| --- | --- |
| `rf-test-domain` (23 tests, 1702 checks) | SHA-256 and CRC-32 against their standard vectors, canonical codec round trips and rejections, UTF-8 validation, overflow-checked arithmetic, identity derivation, RFC 3339 (including pre-1970 fractional instants), freshness, ratio arithmetic, and the **complete** lifecycle transition tables: every (from, to) pair, both directions, with exact expected sets. |
| `rf-test-transport` (9 tests, 1133 checks) | The 48-byte frame header field by field, single-byte-at-a-time decoding equal to batched decoding, a frame exactly at the ceiling, corruption of every header field and of the body digest, decoder latching, and real loopback sockets with a 512 KiB transfer split across many reads. |
| `rf-test-orchestrator` (22 tests, 804 checks) | Lifecycle through the public API; dispatch is not completion; a later stage cannot start before its predecessor gate; blast radius never exceeded; manual gates single-use and generation/revision fenced; stale generation and stale epoch evidence refused; duplicate and reordered evidence change nothing; stale health cannot satisfy a gate; pause stops new work and honours the in-flight policy; abort with and without compensation; rollback failure recorded; canary bounds the first batch; failure budget fails the stage; soak delays the evidence gate; deterministic explanations; serialisation round trip with a flipped byte refused; regeneration; command fencing. |
| `rf-test-multiprocess` (1 test, 19 checks) | **Real processes and real sockets.** A controller daemon is started, two workers attach over loopback TCP, the controller is **killed mid-stage**, a second controller adopts the same journal as a new incarnation, workers reconnect, a manual gate is approved from the status output, and the rollout reaches `completed`. |

Benchmarks (`rf-benchmarks`) measure completed work only: rollouts that finished their stages,
targets carried through their gates, journal records written *and flushed*, records verified
during recovery, and frames decoded from a byte stream. Submission rate is deliberately not
reported.

## Proof surfaces: REAL, SYNTHETIC, UNSUPPORTED

**REAL** - process boundaries, loopback TCP with framed transport, the journal on disk
(including flush and cross-process exclusive locking), process kill and restart, incarnation
fencing, the CLI driving a real daemon, and every unit, adversarial, property and
transition-table check.

**SYNTHETIC** - the *effect* a worker performs. `rf-worker` runs a bounded, genuinely
CPU-consuming computation whose duration and outcome come from a scenario; it does not change
a switch or a device, and no switch, firmware or vendor protocol is exercised anywhere in this
repository. The in-process simulator in `src/sim_adapters.cpp` is a simulator and is labelled
as one everywhere it appears. Replacing the synthetic effect with a device-facing executor
means implementing the same message contract; the orchestration runtime does not change.

**UNSUPPORTED** - nothing here has been run against real switching hardware, a real fabric
controller, RDMA, NVLink, a real fleet, or a multi-host deployment. The control and worker
listeners are loopback-only and carry no message authentication. The POSIX code path in
`src/journal.cpp` is present for portability but is **not compiled or validated** by this
repository's build, which is MSVC on Windows x64.

## Bounds

Every collection is bounded by `RuntimeLimits`: plan size, cohort size, total targets, labels,
selector terms and nesting, attempts per target, evidence per attempt, metrics, dedup and
out-of-order buffers, decision history retained in a snapshot, journal record size, journal
size, snapshot size, frame size, connections, queued frames per connection, workers, dispatch
batch, concurrent attempts, idempotency entries, document size, and the freshness defaults. A
configuration that is internally inconsistent is rejected before the runtime starts.
Externally derived lengths are checked with overflow-checked arithmetic. A rollout snapshot
that would exceed the record ceiling is refused with `resource_exhausted` rather than
truncated, and a plan whose cohort would be truncated is refused rather than run partially.

## Known limitations

* A rollout snapshot stores the retained decision history (64 entries); the journal keeps every
  decision as its own record, so the audit trail is complete even though a snapshot is not.
* The health plane in the process-backed deployment is served by the workers that own the
  targets. A deployment with an independent health plane implements `IHealthAdapter`
  separately; the gate semantics are identical.
* The controller does not spawn workers; supervising them is the deployment's job, which is
  also what lets the test suite kill them.
* `rollout-cli` renders text. Machine-readable state is available through the library
  (`status_to_json`, `decision_to_json`, `cohort_to_json`).

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
