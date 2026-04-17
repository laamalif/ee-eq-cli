#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <csignal>
#include <format>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fstream>
#include <optional>
#include <poll.h>
#include <sys/signalfd.h>
#include <string>
#include <system_error>
#include <ctime>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

#include "app_metadata.hpp"
#include "cli_args.hpp"
#include "daemon_backend.hpp"
#include "daemon_controller.hpp"
#include "daemon_ipc.hpp"
#include "ee_eq_preset_parser.hpp"
#include "logging.hpp"
#include "pipewire_router.hpp"
#include "preset_source.hpp"

namespace {

auto summarize_plugins(const ee::ParsedPreset& preset) -> std::string {
  std::string summary;
  for (size_t i = 0; i < preset.plugin_order.size(); ++i) {
    if (i != 0) {
      summary += ", ";
    }
    summary += preset.plugin_order[i];
  }
  return summary;
}

auto summarize_effective_config(const ee::CliArgs& args, const ee::ParsedPreset& preset) -> std::string {
  const auto sink = args.sink_selector.empty() ? std::string("auto") : args.sink_selector;
  return std::format("effective config: sink={} stages={}", sink, summarize_plugins(preset));
}

auto utc_now_iso8601() -> std::string {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&time, &tm);
  std::array<char, 32> buffer{};
  std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer.data();
}

void print_status_json(const ee::DaemonStatus& status) {
  ee::log::info(nlohmann::json(status).dump(2));
}

auto format_doctor_output(const ee::DaemonStatus& status) -> std::string {
  std::string output = std::format(
      "ee-eq-cli {} (pid {}, up since {})\n"
      "daemon:   {}\n"
      "session:  {}\n"
      "health:   {}",
      status.version, status.pid, status.started_at,
      ee::to_string(status.daemon_state),
      ee::to_string(status.session_state),
      ee::to_string(status.health));

  if (status.effective.session_active && !status.effective.preset_origin.empty()) {
    output += std::format("\npreset:   {}", status.effective.preset_origin);
  } else if (!status.desired.preset_path.empty()) {
    output += std::format("\npreset:   {}", status.desired.preset_path);
  } else {
    output += "\npreset:   (none)";
  }

  if (status.effective.session_active && !status.effective.sink_name.empty()) {
    output += std::format("\nsink:     {} [serial {}]",
                          status.effective.sink_name, status.effective.sink_serial);
  } else {
    output += "\nsink:     (none)";
  }

  output += std::format("\nbypass:   {}", status.desired.bypass ? "on" : "off");

  if (status.effective.session_active) {
    output += std::format("\nvolume:   {}%", static_cast<int>(status.effective.volume * 100.0F));
  }

  if (!status.effective.active_plugins.empty()) {
    std::string plugins;
    for (size_t i = 0; i < status.effective.active_plugins.size(); ++i) {
      if (i != 0) plugins += ", ";
      plugins += status.effective.active_plugins[i];
    }
    output += std::format("\nplugins:  {}", plugins);
  } else {
    output += "\nplugins:  (none)";
  }

  if (!status.last_error.empty()) {
    output += std::format("\nerror:    {}", status.last_error);
  }

  return output;
}

auto wait_for_shutdown_signal(ee::PipeWireRouter& router) -> int {
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  if (sigprocmask(SIG_BLOCK, &signals, nullptr) != 0) {
    throw std::system_error(errno, std::generic_category(), "sigprocmask");
  }

  const int signal_fd = signalfd(-1, &signals, SFD_CLOEXEC);
  if (signal_fd == -1) {
    throw std::system_error(errno, std::generic_category(), "signalfd");
  }

  const int wake_fd = router.wake_fd();

  while (true) {
    std::array<pollfd, 2> poll_fds{{
        {.fd = signal_fd, .events = POLLIN, .revents = 0},
        {.fd = wake_fd, .events = POLLIN, .revents = 0},
    }};

    using TimeoutRep = std::chrono::milliseconds::rep;
    const auto next_timeout = router.next_task_timeout();
    const int timeout_ms = next_timeout.has_value()
                               ? static_cast<int>(std::clamp<TimeoutRep>(next_timeout->count(), 0, INT_MAX))
                               : -1;

    const int poll_result = poll(poll_fds.data(), poll_fds.size(), timeout_ms);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(signal_fd);
      throw std::system_error(errno, std::generic_category(), "poll");
    }

    if (poll_result == 0) {
      router.run_due_tasks();
      continue;
    }

    if ((poll_fds[1].revents & POLLIN) != 0) {
      uint64_t ignored = 0;
      const auto bytes_read = read(wake_fd, &ignored, sizeof(ignored));
      if (bytes_read < 0 && errno != EAGAIN && errno != EINTR) {
        close(signal_fd);
        throw std::system_error(errno, std::generic_category(), "read wake_fd");
      }
      router.run_due_tasks();
    }

    if ((poll_fds[0].revents & POLLIN) != 0) {
      signalfd_siginfo signal_info{};
      if (read(signal_fd, &signal_info, sizeof(signal_info)) == sizeof(signal_info)) {
        close(signal_fd);
        return static_cast<int>(signal_info.ssi_signo);
      }
    }
  }
}

auto handle_daemon_mode(const std::vector<std::string>& arguments) -> std::optional<int> {
  if (arguments.size() < 2) {
    return std::nullopt;
  }

  auto send_request = [&](const ee::DaemonRequest& request) -> std::optional<ee::DaemonResponse> {
    ee::DaemonResponse response;
    std::string error;
    if (!ee::send_daemon_request(request, response, error)) {
      ee::log::error(error);
      return std::nullopt;
    }
    if (!response.ok) {
      ee::log::error(response.error);
      return std::nullopt;
    }
    return response;
  };

  if (arguments[1] == "daemon") {
    if (arguments.size() < 3 || arguments[2] != "start") {
      ee::log::error("usage: ee-eq-cli daemon start [--preset <path>] [--sink <name>]");
      return EXIT_FAILURE;
    }
    if (const auto env_error = ee::daemon_mode_environment_error(); !env_error.empty()) {
      ee::log::error(env_error);
      return EXIT_FAILURE;
    }

    std::string initial_preset;
    std::string initial_sink;
    for (size_t i = 3; i < arguments.size(); ++i) {
      if (arguments[i] == "--preset" || arguments[i] == "-p") {
        if (i + 1 >= arguments.size()) {
          ee::log::error("missing value for --preset");
          return EXIT_FAILURE;
        }
        initial_preset = std::filesystem::absolute(arguments[++i]).string();
      } else if (arguments[i] == "--sink" || arguments[i] == "-s") {
        if (i + 1 >= arguments.size()) {
          ee::log::error("missing value for --sink");
          return EXIT_FAILURE;
        }
        initial_sink = arguments[++i];
      } else {
        ee::log::error(std::format("unknown option: {}", arguments[i]));
        return EXIT_FAILURE;
      }
    }

    if (initial_preset.empty()) {
      if (const char* env = std::getenv("EE_EQ_CLI_DEFAULT_PRESET"); env != nullptr && *env != '\0') {
        initial_preset = env;
      }
    }

    ee::DaemonController controller(
        ee::make_real_session_backend(), ee::kApplicationVersion, static_cast<int>(getpid()), utc_now_iso8601());

    if (!initial_preset.empty()) {
      const auto response = controller.handle_request(
          ee::DaemonRequest{.command = "apply", .preset_path = initial_preset, .sink_selector = initial_sink});
      if (!response.ok) {
        ee::log::error(response.error);
        return EXIT_FAILURE;
      }
    }

    std::string error;
    const int result = ee::run_daemon_ipc_server(controller, error);
    if (result != EXIT_SUCCESS) {
      ee::log::error(error);
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }

  if (arguments[1] == "status") {
    if (const auto response = send_request(ee::DaemonRequest{.command = "status"}); response.has_value()) {
      print_status_json(response->status);
      return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
  }

  if (arguments[1] == "health") {
    if (const auto response = send_request(ee::DaemonRequest{.command = "status"}); response.has_value()) {
      const auto health =
          response->status.health == ee::HealthState::Ok
              ? "ok"
              : response->status.health == ee::HealthState::Degraded ? "degraded" : "failed";
      ee::log::info(health);
      return response->status.health == ee::HealthState::Ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    return EXIT_FAILURE;
  }

  if (arguments[1] == "doctor") {
    if (const auto response = send_request(ee::DaemonRequest{.command = "status"}); response.has_value()) {
      ee::log::info(format_doctor_output(response->status));
      return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
  }

  if (arguments[1] == "current-sink") {
    if (const auto response = send_request(ee::DaemonRequest{.command = "status"}); response.has_value()) {
      if (!response->status.effective.session_active || response->status.effective.sink_name.empty()) {
        ee::log::info("no active session");
      } else {
        ee::log::info(std::format("{} [serial {}]",
                                  response->status.effective.sink_name,
                                  response->status.effective.sink_serial));
      }
      return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
  }

  if (arguments[1] == "apply") {
    if (arguments.size() < 3) {
      ee::log::error("usage: ee-eq-cli apply <preset> [--sink <name-or-serial>]");
      return EXIT_FAILURE;
    }

    ee::DaemonRequest request{.command = "apply", .preset_path = std::filesystem::absolute(arguments[2]).string()};
    for (size_t i = 3; i < arguments.size(); ++i) {
      if (arguments[i] == "--sink" || arguments[i] == "-s") {
        if (i + 1 >= arguments.size()) {
          ee::log::error("missing value for --sink");
          return EXIT_FAILURE;
        }
        request.sink_selector = arguments[++i];
        continue;
      }
      ee::log::error(std::format("unknown option: {}", arguments[i]));
      return EXIT_FAILURE;
    }

    if (const auto response = send_request(request); response.has_value()) {
      print_status_json(response->status);
      return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
  }

  if (arguments[1] == "switch-sink") {
    if (arguments.size() < 3) {
      ee::log::error("usage: ee-eq-cli switch-sink <name-or-serial>");
      return EXIT_FAILURE;
    }
    if (const auto response =
            send_request(ee::DaemonRequest{.command = "switch-sink", .sink_selector = arguments[2]});
        response.has_value()) {
      print_status_json(response->status);
      return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
  }

  if (arguments[1] == "bypass") {
    if (arguments.size() < 3) {
      ee::log::error("usage: ee-eq-cli bypass on|off");
      return EXIT_FAILURE;
    }
    if (const auto response =
            send_request(ee::DaemonRequest{.command = "bypass", .sink_selector = arguments[2]});
        response.has_value()) {
      print_status_json(response->status);
      return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
  }

  if (arguments[1] == "volume") {
    if (arguments.size() < 3) {
      ee::log::error("usage: ee-eq-cli volume <0.0-1.5>");
      return EXIT_FAILURE;
    }
    if (const auto response =
            send_request(ee::DaemonRequest{.command = "volume", .sink_selector = arguments[2]});
        response.has_value()) {
      return response->ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    return EXIT_FAILURE;
  }

  if (arguments[1] == "enable" || arguments[1] == "disable" || arguments[1] == "list-sinks" || arguments[1] == "shutdown") {
    const std::string command =
        arguments[1] == "list-sinks" ? "list-sinks" : arguments[1] == "shutdown" ? "shutdown" : arguments[1];
    if (const auto response = send_request(ee::DaemonRequest{.command = command}); response.has_value()) {
      if (command == "list-sinks") {
        for (const auto& sink : response->sinks) {
          ee::log::info(sink);
        }
      } else {
        print_status_json(response->status);
      }
      return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
  }

  return std::nullopt;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    arguments.emplace_back(argv[i]);
  }

  if (const auto daemon_result = handle_daemon_mode(arguments); daemon_result.has_value()) {
    return *daemon_result;
  }

  std::string error;
  const auto args = ee::parse_cli_args(arguments, error);
  if (!error.empty()) {
    ee::log::error(error);
    return EXIT_FAILURE;
  }
  if (args.show_help) {
    ee::log::info(ee::cli_help_text(argc > 0 ? argv[0] : ee::kApplicationName));
    return EXIT_SUCCESS;
  }
  if (args.show_version) {
    ee::log::info(std::format("{} {}", ee::kApplicationName, ee::kApplicationVersion));
    return EXIT_SUCCESS;
  }

  if (args.list_sinks) {
    ee::PipeWireRouter router(ee::ParsedPreset{}, args.sink_selector);
    std::string router_error;
    const auto sinks = router.list_sinks(router_error);
    if (!router_error.empty()) {
      ee::log::error(router_error);
      return EXIT_FAILURE;
    }
    if (sinks.empty()) {
      ee::log::warn("no PipeWire output sinks were discovered");
      return EXIT_SUCCESS;
    }
    for (const auto& line : sinks) {
      ee::log::info(line);
    }
    return EXIT_SUCCESS;
  }

  if (!args.convert_autoeq_source.empty()) {
    const auto loaded = ee::load_preset_source(args.convert_autoeq_source, error);
    if (!error.empty()) {
      ee::log::error(error);
      return EXIT_FAILURE;
    }

    std::string parse_error;
    const auto preset = ee::parse_autoeq_preset(loaded.bytes, parse_error);
    if (!parse_error.empty()) {
      ee::log::error(parse_error);
      return EXIT_FAILURE;
    }
    for (const auto& warning : preset.warnings) {
      ee::log::warn(warning);
    }

    const auto rendered = ee::render_easy_effects_preset_json(preset);
    if (args.output_path.empty()) {
      ee::log::info(rendered);
      return EXIT_SUCCESS;
    }

    std::ofstream output(args.output_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      ee::log::error(std::format("failed to open output file: {}", args.output_path));
      return EXIT_FAILURE;
    }
    output << rendered << '\n';
    if (!output.good()) {
      ee::log::error(std::format("failed to write output file: {}", args.output_path));
      return EXIT_FAILURE;
    }
    ee::log::info(std::format("wrote converted preset: {}", args.output_path));
    return EXIT_SUCCESS;
  }

  const auto loaded = ee::load_preset_source(args.preset_source, error);
  if (!error.empty()) {
    ee::log::error(error);
    return EXIT_FAILURE;
  }

  std::string parse_error;
  const auto preset = ee::parse_easy_effects_preset(loaded.bytes, parse_error);
  if (!parse_error.empty()) {
    ee::log::error(parse_error);
    return EXIT_FAILURE;
  }

  for (const auto& warning : preset.warnings) {
    ee::log::warn(warning);
  }

  if (args.preset_from_env) {
    ee::log::info(std::format("preset source: {} (from EE_EQ_CLI_DEFAULT_PRESET)", loaded.origin));
  } else {
    ee::log::info(std::format("preset source: {}", loaded.origin));
  }
  ee::log::info(summarize_effective_config(args, preset));
  if (!args.sink_selector.empty()) {
    ee::log::info(std::format("sink override requested: {}", args.sink_selector));
  }

  if (args.dry_run) {
    ee::log::info(std::format("supported plugin order: {}", summarize_plugins(preset)));
    ee::log::info("dry-run complete");
    return EXIT_SUCCESS;
  }

  ee::PipeWireRouter router(preset, args.sink_selector);
  std::string router_error;
  if (!router.start(router_error)) {
    ee::log::error(router_error);
    return EXIT_FAILURE;
  }

  ee::log::info(std::format("ee-eq-cli active with preset: {}", loaded.origin));
  wait_for_shutdown_signal(router);
  router.stop();
  return EXIT_SUCCESS;
}
