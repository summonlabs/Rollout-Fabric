// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/journal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "rollout_fabric/version.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rollout_fabric {
namespace {

inline constexpr std::uint32_t kJournalHeaderBytes = 48;
inline constexpr std::uint32_t kJournalRecordHeaderBytes = 96;

[[nodiscard]] std::uint32_t read_u32(const std::byte* data) noexcept {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[index])) << (index * 8);
  }
  return value;
}

[[nodiscard]] std::uint64_t read_u64(const std::byte* data) noexcept {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data[index])) << (index * 8);
  }
  return value;
}

void write_u32(std::byte* data, std::uint32_t value) noexcept {
  for (unsigned index = 0; index < 4; ++index) {
    data[index] = static_cast<std::byte>(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu));
  }
}

void write_u64(std::byte* data, std::uint64_t value) noexcept {
  for (unsigned index = 0; index < 8; ++index) {
    data[index] = static_cast<std::byte>(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu));
  }
}

[[nodiscard]] Timestamp wall_clock_now() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return Timestamp::from_unix_nanos(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

#if defined(_WIN32)

// A file handle with an explicit sharing mode. A lock handle opened with no
// sharing makes single ownership an operating system guarantee rather than a
// convention a crashed process could leave behind.
class FileHandle {
 public:
  FileHandle() = default;
  ~FileHandle() { close(); }
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;

  [[nodiscard]] Status open(const std::string& path, bool write, bool create, bool exclusive) {
    close();
    std::wstring wide;
    if (!widen(path, wide)) {
      return make_status(StatusCode::kInvalidArgument, "journal path is not valid UTF-8");
    }
    const DWORD share = exclusive ? 0u : (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    const DWORD access = write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    const DWORD disposition = create ? OPEN_ALWAYS : OPEN_EXISTING;
    HANDLE handle = ::CreateFileW(wide.c_str(), access, share, nullptr, disposition,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
        return make_status(StatusCode::kLocked, "the journal is owned by another live process");
      }
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return make_status(StatusCode::kNotFound, "journal file does not exist");
      }
      return make_status(StatusCode::kIoError,
                         "cannot open the journal: Windows error " + std::to_string(error));
    }
    handle_ = handle;
    return Status::success();
  }

  void close() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }

  [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

  [[nodiscard]] Status read_at(std::uint64_t offset, std::span<std::byte> buffer,
                               std::size_t& read_bytes) {
    read_bytes = 0;
    if (buffer.empty()) {
      return Status::success();
    }
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), 1u << 20));
    if (::ReadFile(handle_, buffer.data(), want, &got, &overlapped) == 0) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_HANDLE_EOF) {
        return Status::success();
      }
      return make_status(StatusCode::kIoError,
                         "journal read failed: Windows error " + std::to_string(error));
    }
    read_bytes = got;
    return Status::success();
  }

  [[nodiscard]] Status write_at(std::uint64_t offset, std::span<const std::byte> buffer) {
    std::size_t written_total = 0;
    while (written_total < buffer.size()) {
      OVERLAPPED overlapped{};
      const std::uint64_t position = offset + written_total;
      overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFull);
      overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);
      DWORD written = 0;
      const DWORD want =
          static_cast<DWORD>(std::min<std::size_t>(buffer.size() - written_total, 1u << 20));
      if (::WriteFile(handle_, buffer.data() + written_total, want, &written, &overlapped) == 0) {
        return make_status(StatusCode::kIoError, "journal write failed: Windows error " +
                                                     std::to_string(::GetLastError()));
      }
      if (written == 0) {
        return make_status(StatusCode::kIoError, "journal write made no progress");
      }
      written_total += written;
    }
    return Status::success();
  }

  [[nodiscard]] Status sync() {
    if (::FlushFileBuffers(handle_) == 0) {
      return make_status(StatusCode::kIoError, "journal flush failed: Windows error " +
                                                   std::to_string(::GetLastError()));
    }
    return Status::success();
  }

  [[nodiscard]] Status truncate_at(std::uint64_t size) {
    LARGE_INTEGER distance{};
    distance.QuadPart = static_cast<LONGLONG>(size);
    if (::SetFilePointerEx(handle_, distance, nullptr, FILE_BEGIN) == 0) {
      return make_status(StatusCode::kIoError, "journal seek failed");
    }
    if (::SetEndOfFile(handle_) == 0) {
      return make_status(StatusCode::kIoError, "journal truncate failed");
    }
    return Status::success();
  }

  [[nodiscard]] Status size(std::uint64_t& out) const {
    LARGE_INTEGER value{};
    if (::GetFileSizeEx(handle_, &value) == 0) {
      return make_status(StatusCode::kIoError, "journal size query failed");
    }
    out = static_cast<std::uint64_t>(value.QuadPart);
    return Status::success();
  }

  static bool widen(const std::string& text, std::wstring& out) {
    if (text.empty()) {
      out.clear();
      return true;
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
      return false;
    }
    out.assign(static_cast<std::size_t>(needed), L'\0');
    const int written = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                              static_cast<int>(text.size()), out.data(), needed);
    return written == needed;
  }

  static Status replace_file(const std::string& from, const std::string& to) {
    std::wstring wide_from;
    std::wstring wide_to;
    if (!widen(from, wide_from) || !widen(to, wide_to)) {
      return make_status(StatusCode::kInvalidArgument, "journal path is not valid UTF-8");
    }
    if (::MoveFileExW(wide_from.c_str(), wide_to.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
      return make_status(StatusCode::kIoError, "journal compaction replace failed: Windows error " +
                                                   std::to_string(::GetLastError()));
    }
    return Status::success();
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

#else  // POSIX path. Present for portability; the validated build of this
       // repository is MSVC on Windows x64, so this branch is not exercised
       // here and the README records it as unvalidated.

class FileHandle {
 public:
  FileHandle() = default;
  ~FileHandle() { close(); }
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;

  [[nodiscard]] Status open(const std::string& path, bool write, bool create, bool exclusive) {
    close();
    int flags = write ? (O_RDWR | O_CREAT) : O_RDONLY;
    if (!create) {
      flags &= ~O_CREAT;
    }
    const int descriptor = ::open(path.c_str(), flags, 0644);
    if (descriptor < 0) {
      if (errno == ENOENT) {
        return make_status(StatusCode::kNotFound, "journal file does not exist");
      }
      return make_status(StatusCode::kIoError, "cannot open the journal file");
    }
    if (exclusive && ::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
      ::close(descriptor);
      return make_status(StatusCode::kLocked, "the journal is owned by another live process");
    }
    descriptor_ = descriptor;
    return Status::success();
  }

  void close() noexcept {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
      descriptor_ = -1;
    }
  }

  [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }

  [[nodiscard]] Status read_at(std::uint64_t offset, std::span<std::byte> buffer,
                               std::size_t& read_bytes) {
    read_bytes = 0;
    if (buffer.empty()) {
      return Status::success();
    }
    const ssize_t got =
        ::pread(descriptor_, buffer.data(), buffer.size(), static_cast<off_t>(offset));
    if (got < 0) {
      return make_status(StatusCode::kIoError, "journal read failed");
    }
    read_bytes = static_cast<std::size_t>(got);
    return Status::success();
  }

  [[nodiscard]] Status write_at(std::uint64_t offset, std::span<const std::byte> buffer) {
    std::size_t written_total = 0;
    while (written_total < buffer.size()) {
      const ssize_t written =
          ::pwrite(descriptor_, buffer.data() + written_total, buffer.size() - written_total,
                   static_cast<off_t>(offset + written_total));
      if (written <= 0) {
        return make_status(StatusCode::kIoError, "journal write failed");
      }
      written_total += static_cast<std::size_t>(written);
    }
    return Status::success();
  }

  [[nodiscard]] Status sync() {
    if (::fsync(descriptor_) != 0) {
      return make_status(StatusCode::kIoError, "journal flush failed");
    }
    return Status::success();
  }

  [[nodiscard]] Status truncate_at(std::uint64_t size) {
    if (::ftruncate(descriptor_, static_cast<off_t>(size)) != 0) {
      return make_status(StatusCode::kIoError, "journal truncate failed");
    }
    return Status::success();
  }

  [[nodiscard]] Status size(std::uint64_t& out) const {
    struct stat info {};
    if (::fstat(descriptor_, &info) != 0) {
      return make_status(StatusCode::kIoError, "journal size query failed");
    }
    out = static_cast<std::uint64_t>(info.st_size);
    return Status::success();
  }

  static Status replace_file(const std::string& from, const std::string& to) {
    if (::rename(from.c_str(), to.c_str()) != 0) {
      return make_status(StatusCode::kIoError, "journal compaction replace failed");
    }
    return Status::success();
  }

 private:
  int descriptor_ = -1;
};

#endif

struct JournalHeader {
  JournalId journal_id{};
  std::uint32_t format_version = 0;
  std::uint32_t abi_version = 0;
  Timestamp created_at{};
};

[[nodiscard]] std::array<std::byte, kJournalHeaderBytes> encode_header(const JournalHeader& header) {
  std::array<std::byte, kJournalHeaderBytes> bytes{};
  write_u32(bytes.data(), kJournalMagic);
  write_u32(bytes.data() + 4, header.format_version);
  write_u32(bytes.data() + 8, header.abi_version);
  write_u32(bytes.data() + 12, kJournalHeaderBytes);
  std::memcpy(bytes.data() + 16, header.journal_id.bytes().data(), 16);
  write_u64(bytes.data() + 32, static_cast<std::uint64_t>(header.created_at.unix_nanos()));
  write_u32(bytes.data() + 40, 0);
  write_u32(bytes.data() + 44, crc32(std::span<const std::byte>(bytes.data(), 44)));
  return bytes;
}

[[nodiscard]] Status decode_header(std::span<const std::byte> bytes, JournalHeader& header) {
  if (bytes.size() < kJournalHeaderBytes) {
    return make_status(StatusCode::kCorrupt, "journal header is truncated");
  }
  if (read_u32(bytes.data()) != kJournalMagic) {
    return make_status(StatusCode::kCorrupt, "journal magic does not match");
  }
  header.format_version = read_u32(bytes.data() + 4);
  header.abi_version = read_u32(bytes.data() + 8);
  if (read_u32(bytes.data() + 12) != kJournalHeaderBytes) {
    return make_status(StatusCode::kCorrupt, "journal header declares an unexpected size");
  }
  if (crc32(std::span<const std::byte>(bytes.data(), 44)) != read_u32(bytes.data() + 44)) {
    return make_status(StatusCode::kCorrupt, "journal header checksum does not match");
  }
  if (read_u32(bytes.data() + 40) != 0) {
    return make_status(StatusCode::kCorrupt, "journal header reserved field is not zero");
  }
  header.journal_id = JournalId::from_bytes(
      std::span<const std::byte, JournalId::byte_size>(bytes.data() + 16, JournalId::byte_size));
  header.created_at =
      Timestamp::from_unix_nanos(static_cast<std::int64_t>(read_u64(bytes.data() + 32)));
  if (header.format_version != kSupportedJournalFormatVersion) {
    return make_status(StatusCode::kNotSupported, "journal format version " +
                                                      std::to_string(header.format_version) +
                                                      " is not readable by this build");
  }
  if (header.abi_version != kRuntimeAbiVersion) {
    return make_status(StatusCode::kNotSupported,
                       "journal was written under ABI version " +
                           std::to_string(header.abi_version) + ", this build is " +
                           std::to_string(kRuntimeAbiVersion));
  }
  return Status::success();
}

// Replays the file. valid_end is the offset one past the last record whose
// integrity was proved; everything after it is unprovable and is reported
// rather than repaired into a different meaning.
// Inspects only the framing of the record at the front of a buffer. It answers
// how long this record is without requiring the whole record to be present, which
// is what lets a short read be distinguished from corruption.
[[nodiscard]] Status record_total_length(std::span<const std::byte> bytes,
                                        const RuntimeLimits& limits,
                                        std::uint64_t& total_length) {
  total_length = 0;
  if (bytes.size() < kJournalRecordHeaderBytes) {
    return make_status(StatusCode::kCorrupt, "journal record header is truncated");
  }
  const std::byte* header = bytes.data();
  if (read_u32(header) != kRecordMagic) {
    return make_status(StatusCode::kCorrupt, "journal record magic does not match");
  }
  if (read_u32(header + 4) != kJournalRecordHeaderBytes) {
    return make_status(StatusCode::kCorrupt, "journal record declares an unexpected header size");
  }
  if (crc32(std::span<const std::byte>(header, 92)) != read_u32(header + 92)) {
    return make_status(StatusCode::kCorrupt, "journal record header checksum does not match");
  }
  const std::uint16_t type_raw = static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(header[16]) |
      (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(header[17])) << 8));
  if (type_raw < static_cast<std::uint16_t>(JournalRecordType::kRecordHeader) ||
      type_raw > static_cast<std::uint16_t>(JournalRecordType::kCompactionMarker)) {
    return make_status(StatusCode::kCorrupt, "journal record has an unknown type");
  }
  if (std::to_integer<std::uint8_t>(header[18]) != 0 ||
      std::to_integer<std::uint8_t>(header[19]) != 0) {
    return make_status(StatusCode::kCorrupt, "journal record reserved field is not zero");
  }
  const std::uint32_t payload_length = read_u32(header + 52);
  if (payload_length > limits.max_journal_record_bytes) {
    return make_status(StatusCode::kLimitExceeded,
                       "journal record declares " + std::to_string(payload_length) +
                           " payload bytes, above max_journal_record_bytes");
  }
  total_length = static_cast<std::uint64_t>(kJournalRecordHeaderBytes) + payload_length;
  return Status::success();
}

[[nodiscard]] Status replay(FileHandle& file, std::uint64_t file_size, const RuntimeLimits& limits,
                            std::vector<JournalRecord>* records, bool stop_at_snapshot,
                            std::uint64_t& valid_end, Sequence& last_sequence, bool& have_sequence,
                            RecoveryReport& report) {
  valid_end = 0;
  report.total_bytes = file_size;
  if (file_size == 0) {
    report.empty_journal = true;
    report.detail = "the journal file exists but holds no bytes";
    return Status::success();
  }

  std::vector<std::byte> header_bytes(kJournalHeaderBytes);
  std::size_t read_bytes = 0;
  Status status = file.read_at(0, header_bytes, read_bytes);
  if (!status.ok()) {
    return status;
  }
  if (read_bytes < kJournalHeaderBytes) {
    return make_status(StatusCode::kCorrupt, "journal header is truncated");
  }
  JournalHeader header;
  status = decode_header(header_bytes, header);
  if (!status.ok()) {
    return status;
  }
  report.journal_id = header.journal_id;
  report.format_version = header.format_version;
  report.abi_version = header.abi_version;

  valid_end = kJournalHeaderBytes;
  const std::uint64_t chunk_size =
      std::min<std::uint64_t>(limits.max_journal_record_bytes + kJournalRecordHeaderBytes,
                              8ull << 20);
  std::vector<std::byte> buffer(static_cast<std::size_t>(chunk_size));
  std::vector<std::byte> pending;
  bool stopped = false;

  while (!stopped) {
    if (valid_end + static_cast<std::uint64_t>(pending.size()) < file_size) {
      read_bytes = 0;
      status = file.read_at(valid_end + static_cast<std::uint64_t>(pending.size()), buffer,
                            read_bytes);
      if (!status.ok()) {
        return status;
      }
      if (read_bytes == 0) {
        break;
      }
      pending.insert(pending.end(), buffer.begin(),
                     buffer.begin() + static_cast<std::ptrdiff_t>(read_bytes));
    }

    bool progress = true;
    while (progress && !stopped) {
      progress = false;
      if (pending.size() < kJournalRecordHeaderBytes) {
        break;
      }
      // A short read is not corruption. The record framing is inspected
      // first: a valid header whose payload has not arrived yet means the
      // replay needs more bytes, while a header that does not check out ends
      // the replay for good.
      std::uint64_t total_length = 0;
      const Status framing = record_total_length(pending, limits, total_length);
      if (!framing.ok()) {
        report.detail = framing.message();
        stopped = true;
        break;
      }
      if (static_cast<std::uint64_t>(pending.size()) < total_length) {
        break;
      }
      std::size_t consumed = 0;
      auto record = decode_record_envelope(pending, consumed, limits);
      if (!record.ok()) {
        report.detail = record.status().message();
        stopped = true;
        break;
      }
      if (consumed == 0) {
        report.detail = "journal record decoder made no progress";
        stopped = true;
        break;
      }
      JournalRecord value = std::move(record).value();
      if (have_sequence && !(last_sequence < value.sequence)) {
        report.detail = "journal record sequence " + std::to_string(value_of(value.sequence)) +
                        " does not follow " + std::to_string(value_of(last_sequence));
        ++report.discarded_records;
        stopped = true;
        break;
      }
      last_sequence = value.sequence;
      have_sequence = true;
      ++report.valid_records;
      if (records != nullptr) {
        records->push_back(value);
      }
      pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(consumed));
      valid_end += consumed;
      progress = true;
      if (stop_at_snapshot && value.type == JournalRecordType::kCompactionMarker) {
        stopped = true;
      }
    }
    if (!progress && !stopped &&
        valid_end + static_cast<std::uint64_t>(pending.size()) >= file_size) {
      break;
    }
  }

  report.last_sequence = last_sequence;
  if (valid_end < file_size) {
    report.truncated_tail = true;
    report.discarded_bytes = file_size - valid_end;
    if (report.detail.empty()) {
      report.detail = "trailing bytes do not form a complete, integrity-checked record";
    }
  }
  return Status::success();
}

[[nodiscard]] Status scan_handle(FileHandle& file, const RuntimeLimits& limits,
                                 std::vector<JournalRecord>* records, bool stop_at_snapshot,
                                 std::uint64_t& valid_end, RecoveryReport& report) {
  std::uint64_t file_size = 0;
  const Status status = file.size(file_size);
  if (!status.ok()) {
    return status;
  }
  Sequence last_sequence{};
  bool have_sequence = false;
  return replay(file, file_size, limits, records, stop_at_snapshot, valid_end, last_sequence,
                have_sequence, report);
}

}  // namespace

std::string_view to_string(JournalRecordType type) noexcept {
  switch (type) {
    case JournalRecordType::kRecordHeader: return "record_header";
    case JournalRecordType::kInventoryStored: return "inventory_stored";
    case JournalRecordType::kPlanStored: return "plan_stored";
    case JournalRecordType::kRolloutCreated: return "rollout_created";
    case JournalRecordType::kCohortFrozen: return "cohort_frozen";
    case JournalRecordType::kStageTransition: return "stage_transition";
    case JournalRecordType::kTargetTransition: return "target_transition";
    case JournalRecordType::kAttemptCreated: return "attempt_created";
    case JournalRecordType::kAttemptUpdated: return "attempt_updated";
    case JournalRecordType::kEvidenceRecorded: return "evidence_recorded";
    case JournalRecordType::kDecisionRecorded: return "decision_recorded";
    case JournalRecordType::kRolloutTransition: return "rollout_transition";
    case JournalRecordType::kGateResolved: return "gate_resolved";
    case JournalRecordType::kOperatorCommand: return "operator_command";
    case JournalRecordType::kRolloutSnapshot: return "rollout_snapshot";
    case JournalRecordType::kRolloutRetired: return "rollout_retired";
    case JournalRecordType::kIncarnationClaimed: return "incarnation_claimed";
    case JournalRecordType::kCompactionMarker: return "compaction_marker";
  }
  return "unknown";
}

std::vector<std::byte> encode_record_envelope(const JournalRecord& record,
                                              const RuntimeLimits& limits) {
  std::vector<std::byte> out;
  if (record.payload.size() > limits.max_journal_record_bytes) {
    return out;
  }
  out.resize(kJournalRecordHeaderBytes + record.payload.size());
  std::byte* header = out.data();
  write_u32(header, kRecordMagic);
  write_u32(header + 4, kJournalRecordHeaderBytes);
  write_u64(header + 8, value_of(record.sequence));
  const std::uint16_t type_raw = static_cast<std::uint16_t>(record.type);
  header[16] = static_cast<std::byte>(static_cast<std::uint8_t>(type_raw & 0xFFu));
  header[17] = static_cast<std::byte>(static_cast<std::uint8_t>((type_raw >> 8) & 0xFFu));
  header[18] = std::byte{0};
  header[19] = std::byte{0};
  std::memcpy(header + 20, record.writer.bytes().data(), 16);
  write_u64(header + 36, value_of(record.controller_epoch));
  write_u64(header + 44, static_cast<std::uint64_t>(record.written_at.unix_nanos()));
  write_u32(header + 52, static_cast<std::uint32_t>(record.payload.size()));
  write_u32(header + 56, crc32(record.payload));
  const Digest256 payload_digest =
      is_nil(record.payload_digest) ? sha256(record.payload) : record.payload_digest;
  std::memcpy(header + 60, payload_digest.data(), 32);
  write_u32(header + 92, crc32(std::span<const std::byte>(header, 92)));
  if (!record.payload.empty()) {
    std::memcpy(header + kJournalRecordHeaderBytes, record.payload.data(), record.payload.size());
  }
  return out;
}

Result<JournalRecord> decode_record_envelope(std::span<const std::byte> bytes, std::size_t& consumed,
                                             const RuntimeLimits& limits) {
  consumed = 0;
  if (bytes.size() < kJournalRecordHeaderBytes) {
    return make_status(StatusCode::kCorrupt, "journal record header is truncated");
  }
  const std::byte* header = bytes.data();
  if (read_u32(header) != kRecordMagic) {
    return make_status(StatusCode::kCorrupt, "journal record magic does not match");
  }
  if (read_u32(header + 4) != kJournalRecordHeaderBytes) {
    return make_status(StatusCode::kCorrupt, "journal record declares an unexpected header size");
  }
  if (crc32(std::span<const std::byte>(header, 92)) != read_u32(header + 92)) {
    return make_status(StatusCode::kCorrupt, "journal record header checksum does not match");
  }
  const std::uint16_t type_raw = static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(header[16]) |
      (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(header[17])) << 8));
  if (type_raw < static_cast<std::uint16_t>(JournalRecordType::kRecordHeader) ||
      type_raw > static_cast<std::uint16_t>(JournalRecordType::kCompactionMarker)) {
    return make_status(StatusCode::kCorrupt, "journal record has an unknown type");
  }
  if (std::to_integer<std::uint8_t>(header[18]) != 0 ||
      std::to_integer<std::uint8_t>(header[19]) != 0) {
    return make_status(StatusCode::kCorrupt, "journal record reserved field is not zero");
  }
  const std::uint32_t payload_length = read_u32(header + 52);
  if (payload_length > limits.max_journal_record_bytes) {
    return make_status(StatusCode::kLimitExceeded,
                       "journal record declares " + std::to_string(payload_length) +
                           " payload bytes, above max_journal_record_bytes");
  }
  const std::uint64_t total = static_cast<std::uint64_t>(kJournalRecordHeaderBytes) + payload_length;
  if (bytes.size() < total) {
    return make_status(StatusCode::kCorrupt, "journal record payload is truncated");
  }
  JournalRecord record;
  record.sequence = Sequence{read_u64(header + 8)};
  record.type = static_cast<JournalRecordType>(type_raw);
  record.writer = IncarnationId::from_bytes(
      std::span<const std::byte, IncarnationId::byte_size>(header + 20, IncarnationId::byte_size));
  record.controller_epoch = EpochCounter{read_u64(header + 36)};
  record.written_at = Timestamp::from_unix_nanos(static_cast<std::int64_t>(read_u64(header + 44)));
  record.payload.assign(header + kJournalRecordHeaderBytes, header + total);
  if (crc32(record.payload) != read_u32(header + 56)) {
    return make_status(StatusCode::kCorrupt, "journal record payload checksum does not match");
  }
  Digest256 declared{};
  std::memcpy(declared.data(), header + 60, 32);
  const Digest256 actual = sha256(record.payload);
  if (!digest_equal(declared, actual)) {
    return make_status(StatusCode::kCorrupt, "journal record payload digest does not match");
  }
  record.payload_digest = declared;
  consumed = static_cast<std::size_t>(total);
  return record;
}

std::string RecoveryReport::render() const {
  std::string text;
  text.append("journal ").append(journal_id.to_hex().substr(0, 12));
  text.append(" format=").append(std::to_string(format_version));
  text.append(" abi=").append(std::to_string(abi_version));
  text.append(" records=").append(std::to_string(valid_records));
  text.append(" last_sequence=").append(std::to_string(value_of(last_sequence)));
  text.append(" bytes=").append(std::to_string(total_bytes));
  if (created_new) {
    text.append(" created=yes");
  }
  if (empty_journal) {
    text.append(" empty=yes");
  }
  if (truncated_tail) {
    text.append(" discarded_bytes=").append(std::to_string(discarded_bytes));
    text.append(" rejected_records=").append(std::to_string(discarded_records));
  }
  if (!detail.empty()) {
    text.append(" detail=\"").append(detail).append("\"");
  }
  return text;
}

Result<RecoveryReport> scan_journal(const std::string& path, const RuntimeLimits& limits,
                                    std::vector<JournalRecord>* records, bool stop_at_snapshot) {
  const Status limits_status = limits.validate();
  if (!limits_status.ok()) {
    return limits_status;
  }
  FileHandle file;
  Status status = file.open(path, false, false, false);
  if (!status.ok()) {
    return status;
  }
  if (records != nullptr) {
    records->clear();
  }
  RecoveryReport report;
  std::uint64_t valid_end = 0;
  status = scan_handle(file, limits, records, stop_at_snapshot, valid_end, report);
  if (!status.ok()) {
    return status;
  }
  return report;
}

struct JournalWriter::Impl {
  FileHandle lock;
  FileHandle file;
  std::string path;
  std::string lock_path;
  RuntimeLimits limits{};
};

JournalWriter::~JournalWriter() = default;
JournalWriter::JournalWriter(JournalWriter&& other) noexcept = default;
JournalWriter& JournalWriter::operator=(JournalWriter&& other) noexcept = default;

bool JournalWriter::is_open() const noexcept { return impl_ != nullptr && impl_->file.valid(); }

Result<std::unique_ptr<JournalWriter>> JournalWriter::open(const JournalOpenOptions& options) {
  const Status limits_status = options.limits.validate();
  if (!limits_status.ok()) {
    return limits_status;
  }
  if (options.path.empty()) {
    return make_status(StatusCode::kInvalidArgument, "journal path must not be empty");
  }

  auto writer = std::unique_ptr<JournalWriter>(new JournalWriter());
  writer->impl_ = std::make_unique<Impl>();
  writer->impl_->path = options.path;
  writer->impl_->lock_path = options.path + ".lock";
  writer->impl_->limits = options.limits;

  // The lock is taken first and released last, so ownership survives the
  // close/replace/reopen cycle that compaction performs.
  if (options.exclusive) {
    const Status status = writer->impl_->lock.open(writer->impl_->lock_path, true, true, true);
    if (!status.ok()) {
      return status;
    }
  }

  bool existed = false;
  {
    FileHandle probe;
    existed = probe.open(options.path, false, false, false).ok();
  }
  if (!existed && !options.create_if_missing) {
    return make_status(StatusCode::kNotFound, "journal file does not exist");
  }

  Status status = writer->impl_->file.open(options.path, true, true, false);
  if (!status.ok()) {
    return status;
  }

  RecoveryReport report;
  std::uint64_t valid_end = 0;
  status = scan_handle(writer->impl_->file, options.limits, nullptr, false, valid_end, report);
  if (!status.ok()) {
    return status;
  }

  std::uint64_t file_size = 0;
  status = writer->impl_->file.size(file_size);
  if (!status.ok()) {
    return status;
  }

  if (file_size == 0) {
    IdFactory ids = IdFactory::from_entropy();
    JournalHeader header;
    header.journal_id = ids.next<JournalId>(IdDomain::kJournal);
    header.format_version = options.limits.journal_format_version;
    header.abi_version = kRuntimeAbiVersion;
    header.created_at = wall_clock_now();
    const auto bytes = encode_header(header);
    status = writer->impl_->file.write_at(0, std::span<const std::byte>(bytes.data(), bytes.size()));
    if (!status.ok()) {
      return status;
    }
    status = writer->impl_->file.sync();
    if (!status.ok()) {
      return status;
    }
    writer->journal_id_ = header.journal_id;
    writer->bytes_ = kJournalHeaderBytes;
    writer->last_sequence_ = Sequence{0};
  } else {
    if (report.truncated_tail) {
      if (!options.recover_truncated_tail) {
        return make_status(StatusCode::kCorrupt,
                           "journal has an unprovable tail and recovery is disabled: " +
                               report.detail);
      }
      const Status truncate_status = writer->impl_->file.truncate_at(valid_end);
      if (!truncate_status.ok()) {
        return truncate_status;
      }
      const Status sync_status = writer->impl_->file.sync();
      if (!sync_status.ok()) {
        return sync_status;
      }
    }
    writer->journal_id_ = report.journal_id;
    writer->last_sequence_ = report.last_sequence;
    writer->bytes_ = valid_end;
  }
  writer->records_written_ = 0;
  writer->records_since_snapshot_ = 0;
  return writer;
}

Status JournalWriter::append(JournalRecordType type, EpochCounter controller_epoch,
                             const IncarnationId& writer, Timestamp now,
                             std::span<const std::byte> payload) {
  if (!is_open()) {
    return make_status(StatusCode::kUnavailable, "journal writer is not open");
  }
  JournalRecord record;
  record.sequence = next(last_sequence_);
  record.type = type;
  record.writer = writer;
  record.controller_epoch = controller_epoch;
  record.written_at = now;
  record.payload.assign(payload.begin(), payload.end());
  record.payload_digest = sha256(payload);

  const std::vector<std::byte> bytes = encode_record_envelope(record, impl_->limits);
  if (bytes.empty()) {
    return make_status(StatusCode::kResourceExhausted,
                       "journal record payload exceeds max_journal_record_bytes");
  }
  std::uint64_t projected = 0;
  if (!checked_add(bytes_, static_cast<std::uint64_t>(bytes.size()), projected)) {
    return make_status(StatusCode::kResourceExhausted, "journal size overflowed");
  }
  if (projected > impl_->limits.max_journal_bytes) {
    return make_status(StatusCode::kResourceExhausted,
                       "appending would grow the journal past max_journal_bytes; compact it first");
  }
  Status status =
      impl_->file.write_at(bytes_, std::span<const std::byte>(bytes.data(), bytes.size()));
  if (!status.ok()) {
    return status;
  }
  status = impl_->file.sync();
  if (!status.ok()) {
    return status;
  }
  bytes_ = projected;
  last_sequence_ = record.sequence;
  ++records_written_;
  ++records_since_snapshot_;
  return Status::success();
}

Status JournalWriter::rewrite(const std::vector<JournalRecord>& records) {
  if (!is_open()) {
    return make_status(StatusCode::kUnavailable, "journal writer is not open");
  }
  const std::string temp_path = impl_->path + ".compact";
  FileHandle temp;
  Status status = temp.open(temp_path, true, true, true);
  if (!status.ok()) {
    return status;
  }
  status = temp.truncate_at(0);
  if (!status.ok()) {
    return status;
  }

  JournalHeader header;
  header.journal_id = journal_id_;
  header.format_version = impl_->limits.journal_format_version;
  header.abi_version = kRuntimeAbiVersion;
  header.created_at = wall_clock_now();
  const auto header_bytes = encode_header(header);
  status = temp.write_at(0, std::span<const std::byte>(header_bytes.data(), header_bytes.size()));
  if (!status.ok()) {
    return status;
  }

  std::uint64_t offset = kJournalHeaderBytes;
  Sequence sequence{0};
  const auto write_record = [&](JournalRecordType type, std::span<const std::byte> payload) -> Status {
    JournalRecord record;
    record.sequence = next(sequence);
    record.type = type;
    record.writer = incarnation_;
    record.controller_epoch = controller_epoch_;
    record.written_at = wall_clock_now();
    record.payload.assign(payload.begin(), payload.end());
    record.payload_digest = sha256(payload);
    const std::vector<std::byte> encoded = encode_record_envelope(record, impl_->limits);
    if (encoded.empty()) {
      return make_status(StatusCode::kResourceExhausted,
                         "compacted record exceeds max_journal_record_bytes");
    }
    const Status write_status =
        temp.write_at(offset, std::span<const std::byte>(encoded.data(), encoded.size()));
    if (!write_status.ok()) {
      return write_status;
    }
    offset += encoded.size();
    sequence = record.sequence;
    return Status::success();
  };

  for (const JournalRecord& record : records) {
    status = write_record(record.type, record.payload);
    if (!status.ok()) {
      temp.close();
      FileHandle::replace_file(temp_path, impl_->path);
      return status;
    }
  }
  // A compaction marker closes the file so that a reader asking only for the
  // newest snapshot can stop at a proved boundary.
  status = write_record(JournalRecordType::kCompactionMarker, {});
  if (!status.ok()) {
    temp.close();
    FileHandle::replace_file(temp_path, impl_->path);
    return status;
  }
  status = temp.sync();
  if (!status.ok()) {
    temp.close();
    FileHandle::replace_file(temp_path, impl_->path);
    return status;
  }
  temp.close();

  impl_->file.close();
  status = FileHandle::replace_file(temp_path, impl_->path);
  if (!status.ok()) {
    return status;
  }
  status = impl_->file.open(impl_->path, true, true, false);
  if (!status.ok()) {
    return status;
  }
  bytes_ = offset;
  last_sequence_ = sequence;
  records_since_snapshot_ = 0;
  return Status::success();
}

}  // namespace rollout_fabric
