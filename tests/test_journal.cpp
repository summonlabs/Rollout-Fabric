// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Journal compaction on real files: a compaction that fails part way leaves
// the live journal exactly as it was, and a compaction waits for a reader that
// has the journal open instead of failing and leaving the writer closed.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/journal.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/process.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"
#include "test_harness.hpp"

using namespace rollout_fabric;

namespace {

void remove_journal(const std::string& path) {
  std::error_code ignored;
  for (const char* suffix : {"", ".lock", ".compact"}) {
    std::filesystem::remove(path + suffix, ignored);
  }
}

// A journal path next to the test executable, with no files left from an
// earlier run.
std::string fresh_journal(const std::string& name) {
  auto directory = current_executable_directory();
  const std::filesystem::path base =
      directory.ok() ? std::filesystem::path(directory.value()) : std::filesystem::current_path();
  const std::string path = (base / name).string();
  remove_journal(path);
  return path;
}

std::vector<std::byte> payload(std::size_t size, std::uint8_t fill) {
  return std::vector<std::byte>(size, static_cast<std::byte>(fill));
}

Status append_inventory(JournalWriter& writer, std::uint8_t fill) {
  const std::vector<std::byte> bytes = payload(16, fill);
  return writer.append(JournalRecordType::kInventoryStored, EpochCounter{1}, IncarnationId{},
                       Timestamp::from_unix_nanos(fill), bytes);
}

std::size_t record_count(const std::string& path, const RuntimeLimits& limits) {
  std::vector<JournalRecord> records;
  auto report = scan_journal(path, limits, &records, false);
  return report.ok() ? records.size() : 0;
}

}  // namespace

RF_TEST(failed_compaction_leaves_the_live_journal_intact) {
  const std::string path = fresh_journal("rf-test-journal-failed-compaction.journal");
  JournalOpenOptions options;
  options.path = path;
  auto opened = JournalWriter::open(options);
  RF_REQUIRE(opened.ok());
  std::unique_ptr<JournalWriter> writer = std::move(opened).value();
  RF_REQUIRE(append_inventory(*writer, 1).ok());
  RF_REQUIRE(append_inventory(*writer, 2).ok());
  RF_REQUIRE(append_inventory(*writer, 3).ok());

  // The second snapshot record is one byte over the record ceiling, so the
  // compaction fails after writing the first record to its temporary file.
  std::vector<JournalRecord> snapshot(2);
  snapshot[0].type = JournalRecordType::kInventoryStored;
  snapshot[0].payload = payload(16, 9);
  snapshot[1].type = JournalRecordType::kRolloutSnapshot;
  snapshot[1].payload =
      payload(static_cast<std::size_t>(options.limits.max_journal_record_bytes) + 1, 7);
  const Status failed = writer->rewrite(snapshot);
  RF_CHECK(failed.code() == StatusCode::kResourceExhausted);

  // Nothing was installed: the live journal still holds the three records, and
  // the writer keeps appending to it.
  RF_CHECK_EQ(record_count(path, options.limits), std::size_t{3});
  RF_CHECK(append_inventory(*writer, 4).ok());
  RF_CHECK_EQ(record_count(path, options.limits), std::size_t{4});
  RF_CHECK(!std::filesystem::exists(path + ".compact"));
  writer.reset();
  remove_journal(path);
}

RF_TEST(compaction_waits_for_a_reader_of_the_journal) {
  const std::string path = fresh_journal("rf-test-journal-reader.journal");
  JournalOpenOptions options;
  options.path = path;
  auto opened = JournalWriter::open(options);
  RF_REQUIRE(opened.ok());
  std::unique_ptr<JournalWriter> writer = std::move(opened).value();
  RF_REQUIRE(append_inventory(*writer, 1).ok());
  RF_REQUIRE(append_inventory(*writer, 2).ok());

  std::vector<JournalRecord> snapshot(1);
  snapshot[0].type = JournalRecordType::kInventoryStored;
  snapshot[0].payload = payload(16, 9);
  bool reader_opened = false;
  bool compacted = false;
  {
    // Another tool reading the journal (an inspection, a scan) holds it open
    // while the compaction replaces it, and closes it shortly after.
    std::ifstream reader(path, std::ios::binary);
    reader_opened = reader.is_open();
    std::thread compactor(
        [&writer, &snapshot, &compacted] { compacted = writer->rewrite(snapshot).ok(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    reader.close();
    compactor.join();
  }
  RF_CHECK(reader_opened);
  // The compaction waited for the reader instead of failing with the writer
  // closed.
  RF_CHECK(compacted);
  RF_CHECK(writer->is_open());
  // One snapshot record plus the compaction marker.
  RF_CHECK_EQ(record_count(path, options.limits), std::size_t{2});
  writer.reset();
  remove_journal(path);
}

RF_TEST_MAIN()
