#include "daemon_controller.hpp"

#include <array>
#include <cstdlib>
#include <utility>

#include "app_metadata.hpp"
#include "preset_source.hpp"

namespace ee {

auto daemon_mode_environment_error() -> std::string {
  constexpr std::array kUnsupportedEnv = {
      kDisableConvolverEnv,
      kConvolverRtProcessEnv,
      kConvolverSchedFifoEnv,
  };

  for (const auto* name : kUnsupportedEnv) {
    if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
      return std::string(name) + " is unsupported in daemon mode";
    }
  }
  return {};
}

DaemonController::DaemonController(std::unique_ptr<SessionBackend> backend, std::string version, int pid, std::string started_at)
    : backend_(std::move(backend)) {
  status_.daemon_state = DaemonProcessState::Ready;
  status_.session_state = SessionLifecycleState::Disabled;
  status_.health = HealthState::Ok;
  status_.version = std::move(version);
  status_.pid = pid;
  status_.started_at = std::move(started_at);
}

auto DaemonController::handle_request(const DaemonRequest& request) -> DaemonResponse {
  std::scoped_lock lock(mutex_);

  if (const auto env_error = daemon_mode_environment_error(); !env_error.empty()) {
    return {.ok = false, .error = env_error, .status = status_locked()};
  }

  if (request.command == "status") {
    return {.ok = true, .status = status_locked()};
  }
  if (request.command == "apply") {
    return apply_locked(request);
  }
  if (request.command == "enable") {
    return enable_locked();
  }
  if (request.command == "disable") {
    return disable_locked();
  }
  if (request.command == "switch-sink") {
    return switch_sink_locked(request);
  }
  if (request.command == "bypass") {
    return bypass_locked(request);
  }
  if (request.command == "volume") {
    return volume_locked(request);
  }
  if (request.command == "list-sinks") {
    return list_sinks_locked();
  }
  if (request.command == "shutdown") {
    return shutdown_locked();
  }

  return {.ok = false, .error = "unknown daemon command", .status = status_locked()};
}

void DaemonController::set_shutdown_callback(std::function<void()> callback) {
  std::scoped_lock lock(mutex_);
  shutdown_callback_ = std::move(callback);
}

auto DaemonController::status_locked() const -> DaemonStatus {
  auto status = status_;
  if (desired_.has_value()) {
    status.desired.preset_path = desired_->preset_path;
    status.desired.sink_selector = desired_->sink_selector;
    status.desired.enabled = desired_->enabled;
    status.desired.bypass = desired_->bypass;
    status.desired.volume = desired_->volume;
  } else {
    status.desired = {};
  }

  if (desired_.has_value() && desired_->enabled) {
    const auto snapshot = backend_->snapshot();
    status.effective.session_active = snapshot.session_active;
    status.effective.preset_origin = snapshot.preset_origin;
    status.effective.sink_name = snapshot.sink_name;
    status.effective.sink_serial = snapshot.sink_serial;
    status.effective.active_plugins = snapshot.active_plugins;
    status.effective.bypass = snapshot.bypass;
    status.effective.volume = snapshot.volume;
    if (snapshot.session_active) {
      status.session_state = SessionLifecycleState::Enabled;
      if (!snapshot.init_error.empty()) {
        status.health = HealthState::Degraded;
        status.last_error = snapshot.init_error;
      } else if (init_error_degraded_) {
        status.health = HealthState::Ok;
        status.last_error.clear();
      }
    } else {
      status.session_state = SessionLifecycleState::Degraded;
      status.health = HealthState::Degraded;
    }
  }
  return status;
}

auto DaemonController::apply_locked(const DaemonRequest& request) -> DaemonResponse {
  if (request.preset_path.empty()) {
    return {.ok = false, .error = "apply requires a preset path; usage: eq-cli apply <preset>", .status = status_locked()};
  }

  std::string error;
  const auto loaded = load_preset_source(request.preset_path, error);
  if (!error.empty()) {
    return {.ok = false, .error = error, .status = status_locked()};
  }

  std::string parse_error;
  const auto parsed = parse_easy_effects_preset(loaded.bytes, parse_error);
  if (!parse_error.empty()) {
    return {.ok = false, .error = parse_error, .status = status_locked()};
  }

  DesiredConfig candidate{
      .preset_path = request.preset_path,
      .preset_origin = loaded.origin,
      .preset = parsed,
      .sink_selector = request.sink_selector,
      .enabled = true,
  };

  if (desired_.has_value()) {
    candidate.bypass = desired_->bypass;
    candidate.volume = desired_->volume;
  }

  const auto previous = desired_;

  status_.session_state = SessionLifecycleState::Enabling;

  if (desired_.has_value() && desired_->enabled) {
    backend_->stop_session();
  }

  if (!backend_->start_session(candidate.preset, candidate.preset_origin, candidate.sink_selector, error)) {
    if (previous.has_value() && previous->enabled) {
      std::string rollback_error;
      if (backend_->start_session(previous->preset, previous->preset_origin, previous->sink_selector, rollback_error)) {
        desired_ = previous;
        if (desired_->bypass) {
          backend_->set_bypass(true);
        }
        if (desired_->volume != 1.0F) {
          backend_->set_volume(desired_->volume);
        }
        set_runtime_state_from_backend_locked();
        status_.health = HealthState::Degraded;
        status_.last_error = error;
        return {.ok = false, .error = error, .status = status_locked()};
      }
    }

    desired_ = previous;
    clear_effective_state_locked();
    status_.session_state = SessionLifecycleState::Degraded;
    status_.health = HealthState::Degraded;
    status_.last_error = error;
    return {.ok = false, .error = error, .status = status_locked()};
  }

  desired_ = std::move(candidate);
  if (desired_->bypass) {
    backend_->set_bypass(true);
  }
  if (desired_->volume != 1.0F) {
    backend_->set_volume(desired_->volume);
  }
  set_runtime_state_from_backend_locked();
  status_.health = HealthState::Ok;
  status_.last_error.clear();
  return {.ok = true, .status = status_locked()};
}

auto DaemonController::enable_locked() -> DaemonResponse {
  if (!desired_.has_value()) {
    return {.ok = false, .error = "no preset loaded; run 'eq-cli apply <preset>' first", .status = status_locked()};
  }
  if (desired_->enabled && status_.session_state == SessionLifecycleState::Enabled) {
    return {.ok = true, .status = status_locked()};
  }

  std::string error;
  status_.session_state = SessionLifecycleState::Enabling;
  if (!backend_->start_session(desired_->preset, desired_->preset_origin, desired_->sink_selector, error)) {
    clear_effective_state_locked();
    status_.session_state = SessionLifecycleState::Degraded;
    status_.health = HealthState::Degraded;
    status_.last_error = error;
    desired_->enabled = false;
    return {.ok = false, .error = error, .status = status_locked()};
  }

  desired_->enabled = true;
  if (desired_->bypass) {
    backend_->set_bypass(true);
  }
  if (desired_->volume != 1.0F) {
    backend_->set_volume(desired_->volume);
  }
  set_runtime_state_from_backend_locked();
  status_.health = HealthState::Ok;
  status_.last_error.clear();
  return {.ok = true, .status = status_locked()};
}

auto DaemonController::disable_locked() -> DaemonResponse {
  if (desired_.has_value()) {
    desired_->enabled = false;
  }

  status_.session_state = SessionLifecycleState::Disabling;
  backend_->stop_session();
  clear_effective_state_locked();
  status_.session_state = SessionLifecycleState::Disabled;
  status_.health = HealthState::Ok;
  status_.last_error.clear();
  return {.ok = true, .status = status_locked()};
}

auto DaemonController::list_sinks_locked() -> DaemonResponse {
  std::string error;
  const auto sinks = backend_->list_sinks(error);
  if (!error.empty()) {
    return {.ok = false, .error = error, .status = status_locked()};
  }
  return {.ok = true, .status = status_locked(), .sinks = sinks};
}

auto DaemonController::switch_sink_locked(const DaemonRequest& request) -> DaemonResponse {
  if (!desired_.has_value()) {
    return {.ok = false, .error = "no preset loaded; run 'eq-cli apply <preset>' first", .status = status_locked()};
  }
  if (request.sink_selector.empty()) {
    return {.ok = false, .error = "switch-sink requires a sink name or serial; usage: eq-cli switch-sink <name-or-serial>", .status = status_locked()};
  }

  const auto previous_sink = desired_->sink_selector;
  desired_->sink_selector = request.sink_selector;

  if (!desired_->enabled) {
    return {.ok = true, .status = status_locked()};
  }

  status_.session_state = SessionLifecycleState::Enabling;
  backend_->stop_session();

  std::string error;
  if (!backend_->start_session(desired_->preset, desired_->preset_origin, desired_->sink_selector, error)) {
    desired_->sink_selector = previous_sink;
    std::string rollback_error;
    if (backend_->start_session(desired_->preset, desired_->preset_origin, previous_sink, rollback_error)) {
      if (desired_->bypass) {
        backend_->set_bypass(true);
      }
      if (desired_->volume != 1.0F) {
        backend_->set_volume(desired_->volume);
      }
      set_runtime_state_from_backend_locked();
      status_.health = HealthState::Degraded;
      status_.last_error = error;
      return {.ok = false, .error = error, .status = status_locked()};
    }
    clear_effective_state_locked();
    status_.session_state = SessionLifecycleState::Degraded;
    status_.health = HealthState::Degraded;
    status_.last_error = error;
    return {.ok = false, .error = error, .status = status_locked()};
  }

  if (desired_->bypass) {
    backend_->set_bypass(true);
  }
  if (desired_->volume != 1.0F) {
    backend_->set_volume(desired_->volume);
  }
  set_runtime_state_from_backend_locked();
  status_.health = HealthState::Ok;
  status_.last_error.clear();
  return {.ok = true, .status = status_locked()};
}

auto DaemonController::bypass_locked(const DaemonRequest& request) -> DaemonResponse {
  if (!desired_.has_value() || !desired_->enabled) {
    return {.ok = false, .error = "no active session; run 'eq-cli apply <preset>' to start one", .status = status_locked()};
  }

  const auto& value = request.sink_selector;
  const bool is_on = (value == "on" || value == "true" || value == "1");
  const bool is_off = (value == "off" || value == "false" || value == "0");
  if (!is_on && !is_off) {
    return {.ok = false, .error = "bypass requires on or off; usage: eq-cli bypass on|off", .status = status_locked()};
  }
  desired_->bypass = is_on;
  backend_->set_bypass(is_on);
  status_.health = HealthState::Ok;
  status_.last_error.clear();
  return {.ok = true, .status = status_locked()};
}

auto DaemonController::shutdown_locked() -> DaemonResponse {
  if (desired_.has_value()) {
    desired_->enabled = false;
  }
  status_.daemon_state = DaemonProcessState::Stopping;
  status_.session_state = SessionLifecycleState::Disabling;
  backend_->stop_session();
  clear_effective_state_locked();
  status_.session_state = SessionLifecycleState::Disabled;
  if (shutdown_callback_) {
    shutdown_callback_();
  }
  return {.ok = true, .status = status_locked()};
}

void DaemonController::set_runtime_state_from_backend_locked() {
  const auto snapshot = backend_->snapshot();
  status_.effective.session_active = snapshot.session_active;
  status_.effective.preset_origin = snapshot.preset_origin;
  status_.effective.sink_name = snapshot.sink_name;
  status_.effective.sink_serial = snapshot.sink_serial;
  status_.effective.active_plugins = snapshot.active_plugins;
  status_.effective.bypass = snapshot.bypass;
  status_.effective.volume = snapshot.volume;
  status_.session_state = snapshot.session_active ? SessionLifecycleState::Enabled : SessionLifecycleState::Disabled;
  if (!snapshot.init_error.empty()) {
    status_.health = HealthState::Degraded;
    status_.last_error = snapshot.init_error;
    init_error_degraded_ = true;
  } else if (init_error_degraded_) {
    status_.health = HealthState::Ok;
    status_.last_error.clear();
    init_error_degraded_ = false;
  }
}

auto DaemonController::volume_locked(const DaemonRequest& request) -> DaemonResponse {
  if (!desired_.has_value() || !desired_->enabled) {
    return {.ok = false, .error = "no active session; run 'eq-cli apply <preset>' to start one", .status = status_locked()};
  }

  float value = 0.0F;
  std::size_t pos = 0;
  try {
    value = std::stof(request.sink_selector, &pos);
    if (pos != request.sink_selector.length()) {
      return {.ok = false, .error = "volume requires a number; usage: eq-cli volume <0.0-1.5>", .status = status_locked()};
    }
  } catch (...) {
    return {.ok = false, .error = "volume requires a number; usage: eq-cli volume <0.0-1.5>", .status = status_locked()};
  }

  if (value < 0.0F || value > 1.5F) {
    return {.ok = false, .error = "volume out of range; must be 0.0 to 1.5", .status = status_locked()};
  }

  desired_->volume = value;
  backend_->set_volume(value);
  return {.ok = true, .status = status_locked()};
}

void DaemonController::clear_effective_state_locked() {
  status_.effective = {};
}

}  // namespace ee
