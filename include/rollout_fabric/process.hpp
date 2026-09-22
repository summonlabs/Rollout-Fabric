// Rollout Fabric - real child processes.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The controller supervises worker processes and the integration suite
// supervises controllers. Both need the same thing: start a real operating
// system process, watch it, kill it hard or ask it to stop, and reap it without
// leaking handles. Nothing here is a thread pretending to be a process.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"

namespace rollout_fabric {

struct ProcessSpawnOptions {
  std::string executable;
  std::vector<std::string> arguments;
  std::string working_directory;
  // When true the child inherits this process's console. When false its output
  // is discarded, which is what supervised workers want.
  bool inherit_stdio = false;
  // Captures the child's standard output into a pipe so the parent can read its
  // readiness line. Mutually exclusive with inherit_stdio.
  bool capture_stdout = false;
  // Extra environment entries in "KEY=VALUE" form. The child always inherits
  // the parent environment as well.
  std::vector<std::string> environment;
};

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;

  [[nodiscard]] static Result<ChildProcess> spawn(const ProcessSpawnOptions& options);

  [[nodiscard]] bool running();
  // Hard termination. On Windows this is TerminateProcess, which is exactly the
  // "kill the controller mid-stage" case the runtime must survive.
  [[nodiscard]] Status terminate();
  // Blocking wait for exit. Returns the exit code.
  [[nodiscard]] Result<std::uint32_t> wait();
  // Waits up to the given duration; kDeadlineExceeded means still running.
  [[nodiscard]] Result<std::uint32_t> wait_for(Duration timeout);
  // Reaps without blocking if the process already exited.
  [[nodiscard]] Result<std::uint32_t> try_reap();
  // Signals a graceful stop by writing the control byte to the child's stdin,
  // when the child was spawned with a pipe.
  [[nodiscard]] Status request_stop();
  // Blocking read of one line from the child's captured stdout. No timeout: a
  // child that never becomes ready is a defect, not something to paper over.
  [[nodiscard]] Result<std::string> read_stdout_line();
  // Everything the child has written so far, blocking until it exits.
  [[nodiscard]] Result<std::string> read_stdout_all();

  [[nodiscard]] std::uint32_t pid() const noexcept { return pid_; }
  [[nodiscard]] bool valid() const noexcept { return pid_ != 0; }
  [[nodiscard]] Timestamp started_at() const noexcept { return started_at_; }

 private:
  void close_handles() noexcept;

  std::uintptr_t process_handle_ = 0;
  std::uintptr_t thread_handle_ = 0;
  std::uintptr_t stdin_write_ = 0;
  std::uintptr_t stdout_read_ = 0;
  std::string stdout_pending_;
  std::uint32_t pid_ = 0;
  bool exited_ = false;
  std::uint32_t exit_code_ = 0;
  Timestamp started_at_{};
};

// Resolves the directory of the currently running executable, which is how the
// tools and the test suite locate their sibling binaries.
[[nodiscard]] Result<std::string> current_executable_path();
[[nodiscard]] Result<std::string> current_executable_directory();

}  // namespace rollout_fabric
