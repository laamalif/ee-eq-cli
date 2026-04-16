#include "daemon_protocol.hpp"

#include <nlohmann/json.hpp>

namespace ee {

namespace {

auto to_string(const DaemonProcessState value) -> std::string_view {
  switch (value) {
    case DaemonProcessState::Starting:
      return "starting";
    case DaemonProcessState::Ready:
      return "ready";
    case DaemonProcessState::Stopping:
      return "stopping";
    case DaemonProcessState::Failed:
      return "failed";
  }
  return "failed";
}

auto to_string(const SessionLifecycleState value) -> std::string_view {
  switch (value) {
    case SessionLifecycleState::Disabled:
      return "disabled";
    case SessionLifecycleState::Enabling:
      return "enabling";
    case SessionLifecycleState::Enabled:
      return "enabled";
    case SessionLifecycleState::Disabling:
      return "disabling";
    case SessionLifecycleState::Degraded:
      return "degraded";
  }
  return "degraded";
}

auto to_string(const HealthState value) -> std::string_view {
  switch (value) {
    case HealthState::Ok:
      return "ok";
    case HealthState::Degraded:
      return "degraded";
    case HealthState::Failed:
      return "failed";
  }
  return "failed";
}

auto parse_daemon_state(const std::string& value) -> DaemonProcessState {
  if (value == "ready") {
    return DaemonProcessState::Ready;
  }
  if (value == "starting") {
    return DaemonProcessState::Starting;
  }
  if (value == "stopping") {
    return DaemonProcessState::Stopping;
  }
  return DaemonProcessState::Failed;
}

auto parse_session_state(const std::string& value) -> SessionLifecycleState {
  if (value == "enabled") {
    return SessionLifecycleState::Enabled;
  }
  if (value == "enabling") {
    return SessionLifecycleState::Enabling;
  }
  if (value == "disabling") {
    return SessionLifecycleState::Disabling;
  }
  if (value == "degraded") {
    return SessionLifecycleState::Degraded;
  }
  return SessionLifecycleState::Disabled;
}

auto parse_health_state(const std::string& value) -> HealthState {
  if (value == "ok") {
    return HealthState::Ok;
  }
  if (value == "degraded") {
    return HealthState::Degraded;
  }
  return HealthState::Failed;
}

}  // namespace

auto to_json(nlohmann::json& json, const DaemonStatus& value) -> void {
  json = nlohmann::json{
      {"daemon",
       {
           {"state", to_string(value.daemon_state)},
           {"version", value.version},
           {"pid", value.pid},
           {"started_at", value.started_at},
       }},
      {"session",
       {
           {"state", to_string(value.session_state)},
       }},
      {"desired",
       {
           {"preset_path", value.desired.preset_path},
           {"sink_selector", value.desired.sink_selector},
           {"enabled", value.desired.enabled},
           {"bypass", value.desired.bypass},
       }},
      {"effective",
       {
           {"session_active", value.effective.session_active},
           {"preset_origin", value.effective.preset_origin},
           {"sink_name", value.effective.sink_name},
           {"sink_serial", value.effective.sink_serial},
           {"active_plugins", value.effective.active_plugins},
           {"bypass", value.effective.bypass},
       }},
      {"health", to_string(value.health)},
      {"last_error", value.last_error},
  };
}

auto from_json(const nlohmann::json& json, DaemonStatus& value) -> void {
  const auto& daemon = json.at("daemon");
  const auto& session = json.at("session");
  const auto& desired = json.at("desired");
  const auto& effective = json.at("effective");

  value.daemon_state = parse_daemon_state(daemon.value("state", std::string("failed")));
  value.session_state = parse_session_state(session.value("state", std::string("disabled")));
  value.desired.preset_path = desired.value("preset_path", std::string{});
  value.desired.sink_selector = desired.value("sink_selector", std::string{});
  value.desired.enabled = desired.value("enabled", false);
  value.desired.bypass = desired.value("bypass", false);
  value.effective.session_active = effective.value("session_active", false);
  value.effective.preset_origin = effective.value("preset_origin", std::string{});
  value.effective.sink_name = effective.value("sink_name", std::string{});
  value.effective.sink_serial = effective.value("sink_serial", uint64_t{0});
  value.effective.active_plugins = effective.value("active_plugins", std::vector<std::string>{});
  value.effective.bypass = effective.value("bypass", false);
  value.health = parse_health_state(json.value("health", std::string("failed")));
  value.last_error = json.value("last_error", std::string{});
  value.version = daemon.value("version", std::string{});
  value.pid = daemon.value("pid", 0);
  value.started_at = daemon.value("started_at", std::string{});
}

auto from_json(const nlohmann::json& json, DaemonRequest& value) -> void {
  value.command = json.value("command", std::string{});
  value.preset_path = json.value("preset_path", std::string{});
  value.sink_selector = json.value("sink_selector", std::string{});
}

auto to_json(nlohmann::json& json, const DaemonRequest& value) -> void {
  json = nlohmann::json{
      {"command", value.command},
      {"preset_path", value.preset_path},
      {"sink_selector", value.sink_selector},
  };
}

auto to_json(nlohmann::json& json, const DaemonResponse& value) -> void {
  json = nlohmann::json{
      {"ok", value.ok},
      {"error", value.error},
  };
  if (!value.sinks.empty()) {
    json["sinks"] = value.sinks;
  }
  if (!value.status.version.empty() || value.status.pid != 0 || value.status.daemon_state != DaemonProcessState::Starting ||
      value.status.session_state != SessionLifecycleState::Disabled || !value.status.last_error.empty() ||
      !value.status.desired.preset_path.empty() || !value.status.effective.preset_origin.empty() ||
      value.status.effective.session_active) {
    json["status"] = value.status;
  }
}

auto from_json(const nlohmann::json& json, DaemonResponse& value) -> void {
  value.ok = json.value("ok", false);
  value.error = json.value("error", std::string{});
  value.sinks = json.value("sinks", std::vector<std::string>{});
  if (json.contains("status")) {
    value.status = json.at("status").get<DaemonStatus>();
  }
}

}  // namespace ee
