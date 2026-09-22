// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/process.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include "rollout_fabric/time.hpp"

namespace rollout_fabric {
namespace {

// The exit code a hard kill leaves behind, so a supervisor can tell "the
// controller was killed mid-stage" apart from an ordinary failure.
constexpr std::uint32_t kTerminateExitCode = 0xDEADu;

// Windows path handling is bounded by 32767 wide characters; the retry loop
// never grows past that.
constexpr std::size_t kExecutablePathLimit = 32768;

// Bounded wait used by the destructor after a kill, so a destructor can never
// block forever on a process that refuses to die.
constexpr DWORD kReapTimeoutMillis = 5000;

// Upper bound on one pipe read, and on how much captured output is retained.
constexpr DWORD kReadChunkBytes = 4096;
constexpr std::size_t kMaxPendingLineBytes = 1u << 20;
constexpr std::size_t kMaxCapturedBytes = 16u * (1u << 20);

// Bounded diagnostic text lifted from a partial line.
constexpr std::size_t kMaxQuotedBytes = 256;

constexpr DWORD kMaxWaitMillis = 0xFFFFFFFEu;  // never INFINITE

[[nodiscard]] HANDLE to_handle(std::uintptr_t value) noexcept {
  return reinterpret_cast<HANDLE>(value);
}

[[nodiscard]] std::uintptr_t from_handle(HANDLE value) noexcept {
  return reinterpret_cast<std::uintptr_t>(value);
}

[[nodiscard]] bool is_real_handle(HANDLE handle) noexcept {
  return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

[[nodiscard]] std::string to_utf8(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
  if (needed <= 0) {
    return std::string();
  }
  std::string utf8(static_cast<std::size_t>(needed), '\0');
  const int written = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()), utf8.data(), needed,
                                            nullptr, nullptr);
  if (written <= 0) {
    return std::string();
  }
  utf8.resize(static_cast<std::size_t>(written));
  return utf8;
}

[[nodiscard]] std::wstring to_wide(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  const int written = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()), wide.data(), needed);
  if (written <= 0) {
    return std::wstring();
  }
  wide.resize(static_cast<std::size_t>(written));
  return wide;
}

[[nodiscard]] std::string win32_message(DWORD error) {
  LPWSTR buffer = nullptr;
  const DWORD length = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                            FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, error,
                                        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::string detail;
  if (length != 0 && buffer != nullptr) {
    std::wstring text(buffer, length);
    ::LocalFree(buffer);
    while (!text.empty() &&
           (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
      text.pop_back();
    }
    detail = to_utf8(text);
  }
  if (detail.empty()) {
    return "Win32 error " + std::to_string(error);
  }
  return "Win32 error " + std::to_string(error) + ": " + detail;
}

[[nodiscard]] Status win32_failure(StatusCode code, const char* action) {
  return make_status(code, std::string(action) + " failed: " + win32_message(::GetLastError()));
}

[[nodiscard]] Status win32_failure(StatusCode code, const char* action, DWORD error) {
  return make_status(code, std::string(action) + " failed: " + win32_message(error));
}

// CreateProcessW reports a missing binary, a missing working directory and a
// missing permission with distinct codes; the runtime surfaces those
// distinctions instead of flattening everything into kIoError.
[[nodiscard]] StatusCode spawn_error_code(DWORD error) noexcept {
  switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_DIRECTORY:
    case ERROR_MOD_NOT_FOUND:
      return StatusCode::kNotFound;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
      return StatusCode::kPermissionDenied;
    default:
      return StatusCode::kIoError;
  }
}

// Appends one argument to a command line using the quoting rules
// CommandLineToArgvW (and therefore every C runtime) parses back.
void append_quoted_argument(std::wstring& out, const std::wstring& argument) {
  const bool needs_quotes =
      argument.empty() || argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
  if (!needs_quotes) {
    out.append(argument);
    return;
  }
  out.push_back(L'"');
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
      continue;
    }
    out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(character);
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
}

struct EnvironmentEntry {
  std::wstring name;
  std::wstring value;
};

[[nodiscard]] bool environment_less(const EnvironmentEntry& lhs, const EnvironmentEntry& rhs) {
  const int compared =
      ::CompareStringOrdinal(lhs.name.c_str(), static_cast<int>(lhs.name.size()),
                             rhs.name.c_str(), static_cast<int>(rhs.name.size()), TRUE);
  return compared == CSTR_LESS_THAN;
}

[[nodiscard]] bool environment_same_name(const std::wstring& lhs, const std::wstring& rhs) {
  return ::CompareStringOrdinal(lhs.c_str(), static_cast<int>(lhs.size()), rhs.c_str(),
                                static_cast<int>(rhs.size()), TRUE) == CSTR_EQUAL;
}

// Builds a sorted, double-null terminated Unicode environment block holding the
// parent environment plus the requested overrides.
[[nodiscard]] Result<std::vector<wchar_t>> build_environment_block(
    const std::vector<std::string>& overrides) {
  std::vector<EnvironmentEntry> entries;
  if (LPWCH block = ::GetEnvironmentStringsW()) {
    for (LPWCH cursor = block; *cursor != L'\0'; cursor += std::wcslen(cursor) + 1) {
      const std::wstring entry(cursor);
      const std::size_t split = entry.find(L'=');
      if (split == std::wstring::npos || split == 0) {
        continue;  // "=C:=C:\..." style entries have no usable name
      }
      entries.push_back(EnvironmentEntry{entry.substr(0, split), entry.substr(split + 1)});
    }
    ::FreeEnvironmentStringsW(block);
  }

  for (const std::string& item : overrides) {
    const std::size_t split = item.find('=');
    if (split == std::string::npos || split == 0) {
      return make_status(StatusCode::kInvalidArgument,
                         "environment entry '" + item + "' is not in KEY=VALUE form");
    }
    const std::wstring name = to_wide(item.substr(0, split));
    const std::wstring value = to_wide(item.substr(split + 1));
    if (name.empty()) {
      return make_status(StatusCode::kInvalidArgument,
                         "environment entry '" + item + "' has an empty name");
    }
    bool replaced = false;
    for (EnvironmentEntry& existing : entries) {
      if (environment_same_name(existing.name, name)) {
        existing.value = value;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      entries.push_back(EnvironmentEntry{name, value});
    }
  }

  std::sort(entries.begin(), entries.end(), environment_less);

  std::vector<wchar_t> block;
  for (const EnvironmentEntry& entry : entries) {
    block.insert(block.end(), entry.name.begin(), entry.name.end());
    block.push_back(L'=');
    block.insert(block.end(), entry.value.begin(), entry.value.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return Result<std::vector<wchar_t>>(std::move(block));
}

[[nodiscard]] Result<std::uint32_t> read_exit_code(std::uintptr_t handle) {
  DWORD code = 0;
  if (::GetExitCodeProcess(to_handle(handle), &code) == 0) {
    return win32_failure(StatusCode::kIoError, "GetExitCodeProcess");
  }
  return static_cast<std::uint32_t>(code);
}

void close_if_real(HANDLE& handle) noexcept {
  if (is_real_handle(handle)) {
    ::CloseHandle(handle);
  }
  handle = nullptr;
}

[[nodiscard]] bool pipe_reached_eof(DWORD error) noexcept {
  return error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF ||
         error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA;
}

[[nodiscard]] std::string bounded(const std::string& text) {
  if (text.size() <= kMaxQuotedBytes) {
    return text;
  }
  return text.substr(0, kMaxQuotedBytes) + "...";
}

}  // namespace

ChildProcess::~ChildProcess() {
  if (process_handle_ != 0) {
    DWORD code = 0;
    if (::GetExitCodeProcess(to_handle(process_handle_), &code) != 0 && code == STILL_ACTIVE) {
      ::TerminateProcess(to_handle(process_handle_), kTerminateExitCode);
    }
    ::WaitForSingleObject(to_handle(process_handle_), kReapTimeoutMillis);
  }
  close_handles();
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : process_handle_(other.process_handle_),
      thread_handle_(other.thread_handle_),
      stdin_write_(other.stdin_write_),
      stdout_read_(other.stdout_read_),
      stdout_pending_(std::move(other.stdout_pending_)),
      pid_(other.pid_),
      exited_(other.exited_),
      exit_code_(other.exit_code_),
      started_at_(other.started_at_) {
  other.process_handle_ = 0;
  other.thread_handle_ = 0;
  other.stdin_write_ = 0;
  other.stdout_read_ = 0;
  other.pid_ = 0;
  other.exited_ = false;
  other.exit_code_ = 0;
  other.started_at_ = Timestamp{};
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    // The value being replaced owns a live child; it gets the same treatment the
    // destructor gives it, without allocating a Status.
    if (process_handle_ != 0) {
      DWORD code = 0;
      if (::GetExitCodeProcess(to_handle(process_handle_), &code) != 0 && code == STILL_ACTIVE) {
        ::TerminateProcess(to_handle(process_handle_), kTerminateExitCode);
      }
      ::WaitForSingleObject(to_handle(process_handle_), kReapTimeoutMillis);
    }
    close_handles();

    process_handle_ = other.process_handle_;
    thread_handle_ = other.thread_handle_;
    stdin_write_ = other.stdin_write_;
    stdout_read_ = other.stdout_read_;
    stdout_pending_ = std::move(other.stdout_pending_);
    pid_ = other.pid_;
    exited_ = other.exited_;
    exit_code_ = other.exit_code_;
    started_at_ = other.started_at_;

    other.process_handle_ = 0;
    other.thread_handle_ = 0;
    other.stdin_write_ = 0;
    other.stdout_read_ = 0;
    other.pid_ = 0;
    other.exited_ = false;
    other.exit_code_ = 0;
    other.started_at_ = Timestamp{};
  }
  return *this;
}

void ChildProcess::close_handles() noexcept {
  HANDLE stdin_write = to_handle(stdin_write_);
  HANDLE stdout_read = to_handle(stdout_read_);
  HANDLE thread_handle = to_handle(thread_handle_);
  HANDLE process_handle = to_handle(process_handle_);
  close_if_real(stdin_write);
  close_if_real(stdout_read);
  close_if_real(thread_handle);
  close_if_real(process_handle);
  stdin_write_ = 0;
  stdout_read_ = 0;
  thread_handle_ = 0;
  process_handle_ = 0;
}

Result<ChildProcess> ChildProcess::spawn(const ProcessSpawnOptions& options) {
  if (options.executable.empty()) {
    return make_status(StatusCode::kInvalidArgument, "spawn: the executable path is empty");
  }
  if (options.inherit_stdio && options.capture_stdout) {
    return make_status(StatusCode::kInvalidArgument,
                       "spawn: inherit_stdio and capture_stdout cannot both be requested");
  }

  std::wstring command_line;
  append_quoted_argument(command_line, to_wide(options.executable));
  for (const std::string& argument : options.arguments) {
    command_line.push_back(L' ');
    append_quoted_argument(command_line, to_wide(argument));
  }
  std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
  mutable_command_line.push_back(L'\0');

  SECURITY_ATTRIBUTES inheritable{};
  inheritable.nLength = static_cast<DWORD>(sizeof(inheritable));
  inheritable.bInheritHandle = TRUE;
  inheritable.lpSecurityDescriptor = nullptr;

  // Handles owned by the child. They are closed in the parent as soon as the
  // process exists, which is what makes end-of-file observable.
  HANDLE child_stdin = nullptr;
  HANDLE child_stdout = nullptr;
  HANDLE null_read = nullptr;
  HANDLE null_write = nullptr;
  // Handles owned by this object.
  HANDLE parent_stdin_write = nullptr;
  HANDLE parent_stdout_read = nullptr;

  const auto release_child_ends = [&child_stdin, &child_stdout, &null_read, &null_write]() {
    close_if_real(child_stdin);
    close_if_real(child_stdout);
    close_if_real(null_read);
    close_if_real(null_write);
  };
  const auto release_parent_ends = [&parent_stdin_write, &parent_stdout_read]() {
    close_if_real(parent_stdin_write);
    close_if_real(parent_stdout_read);
  };

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  DWORD flags = 0;
  BOOL inherit_handles = FALSE;

  if (options.capture_stdout) {
    if (::CreatePipe(&parent_stdout_read, &child_stdout, &inheritable, 0) == 0) {
      return win32_failure(StatusCode::kIoError, "CreatePipe(stdout)");
    }
    if (::CreatePipe(&child_stdin, &parent_stdin_write, &inheritable, 0) == 0) {
      const Status failure = win32_failure(StatusCode::kIoError, "CreatePipe(stdin)");
      release_child_ends();
      release_parent_ends();
      return failure;
    }
    // The parent's ends must not leak into the child, or the child would keep
    // its own output pipe open forever.
    if (::SetHandleInformation(parent_stdout_read, HANDLE_FLAG_INHERIT, 0) == 0 ||
        ::SetHandleInformation(parent_stdin_write, HANDLE_FLAG_INHERIT, 0) == 0) {
      const Status failure = win32_failure(StatusCode::kIoError, "SetHandleInformation");
      release_child_ends();
      release_parent_ends();
      return failure;
    }
    startup.dwFlags |= STARTF_USESTDHANDLES;
    startup.hStdInput = child_stdin;
    startup.hStdOutput = child_stdout;
    startup.hStdError = child_stdout;
    inherit_handles = TRUE;
    flags |= CREATE_NO_WINDOW;
  } else if (options.inherit_stdio) {
    startup.dwFlags |= STARTF_USESTDHANDLES;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    inherit_handles = TRUE;
  } else {
    // Nothing the child writes is wanted, and it must not borrow the parent's
    // console: give it the null device instead.
    null_read = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &inheritable, OPEN_EXISTING, 0, nullptr);
    null_write = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &inheritable, OPEN_EXISTING, 0, nullptr);
    if (is_real_handle(null_read) && is_real_handle(null_write)) {
      startup.dwFlags |= STARTF_USESTDHANDLES;
      startup.hStdInput = null_read;
      startup.hStdOutput = null_write;
      startup.hStdError = null_write;
      inherit_handles = TRUE;
      flags |= CREATE_NO_WINDOW;
    } else {
      // Falling back to "no inherited handles" still discards output, because
      // no console handles are passed on at all.
      release_child_ends();
      startup.dwFlags = 0;
      startup.hStdInput = nullptr;
      startup.hStdOutput = nullptr;
      startup.hStdError = nullptr;
      inherit_handles = FALSE;
    }
  }

  std::vector<wchar_t> environment_block;
  LPVOID environment_pointer = nullptr;
  if (!options.environment.empty()) {
    auto block = build_environment_block(options.environment);
    if (!block.ok()) {
      release_child_ends();
      release_parent_ends();
      return block.status();
    }
    environment_block = std::move(block).value();
    environment_pointer = environment_block.data();
    flags |= CREATE_UNICODE_ENVIRONMENT;
  }

  const std::wstring working_directory = to_wide(options.working_directory);
  const wchar_t* working_directory_pointer =
      working_directory.empty() ? nullptr : working_directory.c_str();

  PROCESS_INFORMATION process_info{};
  const BOOL created = ::CreateProcessW(nullptr, mutable_command_line.data(), nullptr, nullptr,
                                        inherit_handles, flags, environment_pointer,
                                        working_directory_pointer, &startup, &process_info);
  if (created == 0) {
    const DWORD error = ::GetLastError();
    release_child_ends();
    release_parent_ends();
    return make_status(spawn_error_code(error),
                       "spawn '" + options.executable + "' failed: " + win32_message(error));
  }

  ChildProcess child;
  child.process_handle_ = from_handle(process_info.hProcess);
  child.thread_handle_ = from_handle(process_info.hThread);
  child.stdin_write_ = from_handle(parent_stdin_write);
  child.stdout_read_ = from_handle(parent_stdout_read);
  child.pid_ = process_info.dwProcessId;
  child.started_at_ = SystemClock{}.wall_now();

  parent_stdin_write = nullptr;
  parent_stdout_read = nullptr;
  release_child_ends();
  return Result<ChildProcess>(std::move(child));
}

bool ChildProcess::running() {
  if (process_handle_ == 0 || exited_) {
    return false;
  }
  const DWORD waited = ::WaitForSingleObject(to_handle(process_handle_), 0);
  if (waited == WAIT_TIMEOUT) {
    return true;
  }
  if (waited != WAIT_OBJECT_0) {
    return false;
  }
  auto code = read_exit_code(process_handle_);
  if (!code.ok()) {
    return false;
  }
  exited_ = true;
  exit_code_ = code.value();
  return false;
}

Status ChildProcess::terminate() {
  if (process_handle_ == 0) {
    return make_status(StatusCode::kInvalidArgument, "terminate: no process is attached");
  }
  if (exited_) {
    return Status::success();
  }
  if (::TerminateProcess(to_handle(process_handle_), kTerminateExitCode) == 0) {
    return win32_failure(StatusCode::kIoError, "TerminateProcess");
  }
  return Status::success();
}

Result<std::uint32_t> ChildProcess::wait() {
  if (process_handle_ == 0) {
    return make_status(StatusCode::kInvalidArgument, "wait: no process is attached");
  }
  if (exited_) {
    return exit_code_;
  }
  const DWORD waited = ::WaitForSingleObject(to_handle(process_handle_), INFINITE);
  if (waited != WAIT_OBJECT_0) {
    return win32_failure(StatusCode::kIoError, "WaitForSingleObject");
  }
  auto code = read_exit_code(process_handle_);
  if (!code.ok()) {
    return code.status();
  }
  exited_ = true;
  exit_code_ = code.value();
  return exit_code_;
}

Result<std::uint32_t> ChildProcess::wait_for(Duration timeout) {
  if (process_handle_ == 0) {
    return make_status(StatusCode::kInvalidArgument, "wait_for: no process is attached");
  }
  if (exited_) {
    return exit_code_;
  }
  DWORD millis = 0;
  if (timeout.nanos() > 0) {
    const std::int64_t requested = timeout.millis();
    if (requested >= static_cast<std::int64_t>(kMaxWaitMillis)) {
      millis = kMaxWaitMillis;
    } else {
      millis = static_cast<DWORD>(requested);
      if (millis == 0) {
        millis = 1;  // a sub-millisecond wait still yields at least one tick
      }
    }
  }
  const DWORD waited = ::WaitForSingleObject(to_handle(process_handle_), millis);
  if (waited == WAIT_TIMEOUT) {
    return make_status(StatusCode::kDeadlineExceeded, "wait_for: the process is still running");
  }
  if (waited != WAIT_OBJECT_0) {
    return win32_failure(StatusCode::kIoError, "WaitForSingleObject");
  }
  auto code = read_exit_code(process_handle_);
  if (!code.ok()) {
    return code.status();
  }
  exited_ = true;
  exit_code_ = code.value();
  return exit_code_;
}

Result<std::uint32_t> ChildProcess::try_reap() {
  if (process_handle_ == 0) {
    return make_status(StatusCode::kInvalidArgument, "try_reap: no process is attached");
  }
  if (exited_) {
    return exit_code_;
  }
  const DWORD waited = ::WaitForSingleObject(to_handle(process_handle_), 0);
  if (waited == WAIT_TIMEOUT) {
    return make_status(StatusCode::kUnavailable, "try_reap: the process is still running");
  }
  if (waited != WAIT_OBJECT_0) {
    return win32_failure(StatusCode::kIoError, "WaitForSingleObject");
  }
  auto code = read_exit_code(process_handle_);
  if (!code.ok()) {
    return code.status();
  }
  exited_ = true;
  exit_code_ = code.value();
  return exit_code_;
}

Status ChildProcess::request_stop() {
  if (stdin_write_ == 0) {
    return make_status(StatusCode::kUnavailable,
                       "request_stop: the child was not spawned with a stdin pipe");
  }
  const char control = '\n';
  DWORD written = 0;
  if (::WriteFile(to_handle(stdin_write_), &control, 1, &written, nullptr) == 0) {
    return win32_failure(StatusCode::kIoError, "WriteFile(stdin)");
  }
  if (written != 1) {
    return make_status(StatusCode::kIoError,
                       "request_stop: the control byte was not accepted by the child's stdin");
  }
  return Status::success();
}

Result<std::string> ChildProcess::read_stdout_line() {
  if (stdout_read_ == 0) {
    return make_status(StatusCode::kUnavailable, "read_stdout_line: stdout was not captured");
  }
  for (;;) {
    const std::size_t newline = stdout_pending_.find('\n');
    if (newline != std::string::npos) {
      std::string line = stdout_pending_.substr(0, newline);
      stdout_pending_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return line;
    }

    char buffer[kReadChunkBytes];
    DWORD read = 0;
    if (::ReadFile(to_handle(stdout_read_), buffer, sizeof(buffer), &read, nullptr) == 0) {
      const DWORD error = ::GetLastError();
      if (!pipe_reached_eof(error)) {
        return win32_failure(StatusCode::kIoError, "ReadFile(stdout)", error);
      }
      read = 0;
    }
    if (read == 0) {
      if (!stdout_pending_.empty()) {
        return make_status(StatusCode::kUnavailable,
                           "read_stdout_line: stdout closed mid-line; buffered text: " +
                               bounded(stdout_pending_));
      }
      return make_status(StatusCode::kUnavailable,
                         "read_stdout_line: stdout closed with no complete line pending");
    }
    if (stdout_pending_.size() + read > kMaxPendingLineBytes) {
      return make_status(StatusCode::kResourceExhausted,
                         "read_stdout_line: the child produced more than " +
                             std::to_string(kMaxPendingLineBytes) +
                             " byte(s) without a newline");
    }
    stdout_pending_.append(buffer, static_cast<std::size_t>(read));
  }
}

Result<std::string> ChildProcess::read_stdout_all() {
  if (stdout_read_ == 0) {
    return make_status(StatusCode::kUnavailable, "read_stdout_all: stdout was not captured");
  }
  std::string collected = stdout_pending_;
  for (;;) {
    char buffer[kReadChunkBytes];
    DWORD read = 0;
    if (::ReadFile(to_handle(stdout_read_), buffer, sizeof(buffer), &read, nullptr) == 0) {
      const DWORD error = ::GetLastError();
      if (!pipe_reached_eof(error)) {
        return win32_failure(StatusCode::kIoError, "ReadFile(stdout)", error);
      }
      break;
    }
    if (read == 0) {
      break;  // end of the pipe
    }
    if (collected.size() + read > kMaxCapturedBytes) {
      return make_status(StatusCode::kResourceExhausted,
                         "read_stdout_all: the child produced more than " +
                             std::to_string(kMaxCapturedBytes) + " byte(s) of output");
    }
    collected.append(buffer, static_cast<std::size_t>(read));
  }
  stdout_pending_.clear();
  return collected;
}

Result<std::string> current_executable_path() {
  std::vector<wchar_t> buffer(512);
  for (int attempt = 0; attempt < 8; ++attempt) {
    const DWORD written =
        ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0) {
      return win32_failure(StatusCode::kIoError, "GetModuleFileNameW");
    }
    if (static_cast<std::size_t>(written) < buffer.size()) {
      return to_utf8(std::wstring(buffer.data(), static_cast<std::size_t>(written)));
    }
    if (buffer.size() >= kExecutablePathLimit) {
      break;
    }
    buffer.resize(std::min(buffer.size() * 2, kExecutablePathLimit));
  }
  return make_status(StatusCode::kLimitExceeded,
                     "GetModuleFileNameW: the executable path exceeds " +
                         std::to_string(kExecutablePathLimit) + " characters");
}

Result<std::string> current_executable_directory() {
  auto path = current_executable_path();
  if (!path.ok()) {
    return path.status();
  }
  const std::string& full = path.value();
  const std::size_t separator = full.find_last_of("\\/");
  if (separator == std::string::npos) {
    return std::string(".");
  }
  if (separator == 0) {
    return full.substr(0, 1);  // the root of the current drive
  }
  if (separator == 2 && full[1] == ':') {
    return full.substr(0, 3);  // "C:\app.exe" keeps the root separator
  }
  return full.substr(0, separator);
}

}  // namespace rollout_fabric
