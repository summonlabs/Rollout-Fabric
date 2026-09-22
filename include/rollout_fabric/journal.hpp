// Rollout Fabric - durable, integrity checked, append-only progress storage.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Layout:
//
//   [ file header ][ record 1 ][ record 2 ] ... [ record N ][ partial tail ]
//
// The header pins the format version, the ABI version and the journal identity,
// and is covered by its own checksum. Every record carries its sequence number,
// its writer incarnation, the controller epoch it was written under, its
// payload length, a CRC-32 over the framing and a SHA-256 of the payload.
//
// Recovery is conservative in exactly one direction: it keeps everything it can
// prove and discards everything it cannot. A record that is truncated, whose
// CRC fails, whose payload digest fails, whose sequence is not strictly
// increasing, or whose length exceeds the configured ceiling ends the replay.
// The valid prefix is preserved; the unprovable suffix is discarded, counted
// and reported. Nothing is ever silently repaired into a different meaning.
//
// A second controller cannot open the same journal: the file is held open with
// no sharing, so ownership is enforced by the operating system rather than by a
// convention that a crashed process could leave behind.
#pragma once

#include <cstdint>
#include <memory>
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

inline constexpr std::uint32_t kJournalFormatVersion = 1;
inline constexpr std::uint32_t kJournalMagic = 0x4A465231u;  // 'JFR1'
inline constexpr std::uint32_t kRecordMagic = 0x52454331u;   // 'REC1'

enum class JournalRecordType : std::uint16_t {
  kRecordHeader = 1,
  kInventoryStored = 2,
  kPlanStored = 3,
  kRolloutCreated = 4,
  kCohortFrozen = 5,
  kStageTransition = 6,
  kTargetTransition = 7,
  kAttemptCreated = 8,
  kAttemptUpdated = 9,
  kEvidenceRecorded = 10,
  kDecisionRecorded = 11,
  kRolloutTransition = 12,
  kGateResolved = 13,
  kOperatorCommand = 14,
  kRolloutSnapshot = 15,
  kRolloutRetired = 16,
  kIncarnationClaimed = 17,
  kCompactionMarker = 18,
};

[[nodiscard]] std::string_view to_string(JournalRecordType type) noexcept;

struct JournalRecord {
  Sequence sequence{};
  JournalRecordType type = JournalRecordType::kRecordHeader;
  IncarnationId writer{};
  EpochCounter controller_epoch{0};
  Timestamp written_at{};
  Digest256 payload_digest{};
  std::vector<std::byte> payload;
};

struct JournalOpenOptions {
  std::string path;
  RuntimeLimits limits{};
  bool create_if_missing = true;
  // When false a damaged tail is an error instead of a truncation.
  bool recover_truncated_tail = true;
  // Exclusive ownership: refuse to open a journal another live process owns.
  bool exclusive = true;
};

struct RecoveryReport {
  JournalId journal_id{};
  std::uint32_t format_version = 0;
  std::uint32_t abi_version = 0;
  Sequence last_sequence{};
  std::uint64_t valid_records = 0;
  std::uint64_t total_bytes = 0;
  std::uint64_t discarded_bytes = 0;
  std::uint64_t discarded_records = 0;
  bool created_new = false;
  bool truncated_tail = false;
  bool empty_journal = false;
  std::string detail;

  [[nodiscard]] std::string render() const;
};

// Reads a journal without taking ownership. Used by the inspection tool and by
// recovery-before-claim.
[[nodiscard]] Result<RecoveryReport> scan_journal(const std::string& path,
                                                  const RuntimeLimits& limits,
                                                  std::vector<JournalRecord>* records,
                                                  bool stop_at_snapshot);

class JournalWriter {
 public:
  JournalWriter() = default;
  ~JournalWriter();
  JournalWriter(const JournalWriter&) = delete;
  JournalWriter& operator=(const JournalWriter&) = delete;
  JournalWriter(JournalWriter&& other) noexcept;
  JournalWriter& operator=(JournalWriter&& other) noexcept;

  static Result<std::unique_ptr<JournalWriter>> open(const JournalOpenOptions& options);

  // Appends and flushes one record. The record is durable before the call
  // returns: an orchestration decision that has been published has already been
  // persisted, so a crash can never lose a decision that was acted upon.
  [[nodiscard]] Status append(JournalRecordType type,
                              EpochCounter controller_epoch,
                              const IncarnationId& writer,
                              Timestamp now,
                              std::span<const std::byte> payload);

  // Rewrites the file so that it contains exactly the supplied snapshot
  // records followed by a compaction marker, and nothing else. Sequences are
  // renumbered from one. Performed through a temporary file and an atomic
  // replace, so an interrupted compaction leaves the original journal intact
  // and a partially written temporary is never observed as a journal.
  [[nodiscard]] Status rewrite(const std::vector<JournalRecord>& records);

  // Incarnation and epoch recorded on every record this writer appends,
  // including the ones a compaction writes.
  void set_writer_identity(const IncarnationId& writer, EpochCounter controller_epoch) noexcept {
    incarnation_ = writer;
    controller_epoch_ = controller_epoch;
  }

  [[nodiscard]] Sequence last_sequence() const noexcept { return last_sequence_; }
  [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::uint64_t records_written() const noexcept { return records_written_; }
  [[nodiscard]] const JournalId& journal_id() const noexcept { return journal_id_; }
  [[nodiscard]] std::uint64_t records_since_snapshot() const noexcept { return records_since_snapshot_; }
  void note_snapshot() noexcept { records_since_snapshot_ = 0; }
  [[nodiscard]] bool is_open() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  JournalId journal_id_{};
  IncarnationId incarnation_{};
  EpochCounter controller_epoch_{0};
  Sequence last_sequence_{};
  std::uint64_t bytes_ = 0;
  std::uint64_t records_written_ = 0;
  std::uint64_t records_since_snapshot_ = 0;
};

// Encodes and decodes the standard record envelope, exposed so tests can forge
// records with correct framing and then corrupt exactly one field.
[[nodiscard]] std::vector<std::byte> encode_record_envelope(const JournalRecord& record,
                                                            const RuntimeLimits& limits);
[[nodiscard]] Result<JournalRecord> decode_record_envelope(std::span<const std::byte> bytes,
                                                           std::size_t& consumed,
                                                           const RuntimeLimits& limits);

}  // namespace rollout_fabric
