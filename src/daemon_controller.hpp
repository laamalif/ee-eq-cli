#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "daemon_backend.hpp"
#include "daemon_protocol.hpp"

namespace ee {

auto daemon_mode_environment_error() -> std::string;

class DaemonController {
 public:
  DaemonController(std::unique_ptr<SessionBackend> backend, std::string version, int pid, std::string started_at);

  auto handle_request(const DaemonRequest& request) -> DaemonResponse;
  void set_shutdown_callback(std::function<void()> callback);

 private:
  struct DesiredConfig {
    std::string preset_path;
    std::string preset_origin;
    ParsedPreset preset;
    std::string sink_selector;
    bool enabled = false;
  };

  auto status_locked() const -> DaemonStatus;
  auto apply_locked(const DaemonRequest& request) -> DaemonResponse;
  auto enable_locked() -> DaemonResponse;
  auto disable_locked() -> DaemonResponse;
  auto list_sinks_locked() -> DaemonResponse;
  auto shutdown_locked() -> DaemonResponse;
  void set_runtime_state_from_backend_locked();
  void clear_effective_state_locked();

  std::unique_ptr<SessionBackend> backend_;
  mutable std::mutex mutex_;
  DaemonStatus status_;
  std::optional<DesiredConfig> desired_;
  std::function<void()> shutdown_callback_;
};

}  // namespace ee
