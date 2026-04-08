#include <algorithm>
#include <array>
#include <cstdlib>
#include <csignal>
#include <format>
#include <cerrno>
#include <climits>
#include <cstring>
#include <poll.h>
#include <sys/signalfd.h>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "cli_args.hpp"
#include "ee_eq_preset_parser.hpp"
#include "logging.hpp"
#include "pipewire_router.hpp"
#include "preset_source.hpp"

namespace {

constexpr auto kApplicationName = "ee-eq-cli";
constexpr auto kApplicationVersion = "0.2.9.5";

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

}  // namespace

int main(int argc, char* argv[]) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    arguments.emplace_back(argv[i]);
  }

  std::string error;
  const auto args = ee::parse_cli_args(arguments, error);
  if (!error.empty()) {
    ee::log::error(error);
    return EXIT_FAILURE;
  }
  if (args.show_help) {
    ee::log::info(ee::cli_help_text(argc > 0 ? argv[0] : kApplicationName));
    return EXIT_SUCCESS;
  }
  if (args.show_version) {
    ee::log::info(std::format("{} {}", kApplicationName, kApplicationVersion));
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
