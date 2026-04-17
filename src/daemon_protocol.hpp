#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace ee {

enum class DaemonProcessState {
  Starting,
  Ready,
  Stopping,
  Failed,
};

enum class SessionLifecycleState {
  Disabled,
  Enabling,
  Enabled,
  Disabling,
  Degraded,
};

enum class HealthState {
  Ok,
  Degraded,
  Failed,
};

auto to_string(DaemonProcessState value) -> std::string_view;
auto to_string(SessionLifecycleState value) -> std::string_view;
auto to_string(HealthState value) -> std::string_view;

struct DesiredState {
  std::string preset_path;
  std::string sink_selector;
  bool enabled = false;
  bool bypass = false;
  float volume = 1.0F;
};

struct EffectiveState {
  bool session_active = false;
  std::string preset_origin;
  std::string sink_name;
  uint64_t sink_serial = 0;
  std::vector<std::string> active_plugins;
  bool bypass = false;
  float volume = 1.0F;
};

struct DaemonStatus {
  DaemonProcessState daemon_state = DaemonProcessState::Starting;
  SessionLifecycleState session_state = SessionLifecycleState::Disabled;
  DesiredState desired;
  EffectiveState effective;
  HealthState health = HealthState::Ok;
  std::string last_error;
  std::string version;
  int pid = 0;
  std::string started_at;
};

struct DaemonRequest {
  std::string command;
  std::string preset_path;
  std::string sink_selector;
};

struct DaemonResponse {
  bool ok = false;
  std::string error;
  DaemonStatus status;
  std::vector<std::string> sinks;
};

auto to_json(nlohmann::json& json, const DaemonStatus& value) -> void;
auto from_json(const nlohmann::json& json, DaemonStatus& value) -> void;
auto from_json(const nlohmann::json& json, DaemonRequest& value) -> void;
auto to_json(nlohmann::json& json, const DaemonRequest& value) -> void;
auto to_json(nlohmann::json& json, const DaemonResponse& value) -> void;
auto from_json(const nlohmann::json& json, DaemonResponse& value) -> void;

}  // namespace ee
