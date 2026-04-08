#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ee {

struct CliArgs {
  std::string preset_source;
  std::string sink_selector;
  bool dry_run = false;
  bool list_sinks = false;
  bool show_help = false;
  bool show_version = false;
  bool preset_from_env = false;
};

auto parse_cli_args(std::span<const std::string> arguments, std::string& error) -> CliArgs;
auto cli_help_text(std::string_view executable_name) -> std::string;

}  // namespace ee
