#include "cli_args.hpp"

#include <cctype>
#include <cstdlib>
#include <format>

#include "app_metadata.hpp"

namespace ee {

namespace {

auto is_url(std::string_view value) -> bool {
  auto starts_with_ci = [value](std::string_view prefix) {
    if (value.size() < prefix.size()) {
      return false;
    }

    for (size_t i = 0; i < prefix.size(); ++i) {
      const auto lhs = static_cast<unsigned char>(value[i]);
      const auto rhs = static_cast<unsigned char>(prefix[i]);
      if (std::tolower(lhs) != std::tolower(rhs)) {
        return false;
      }
    }
    return true;
  };

  return starts_with_ci("http://") || starts_with_ci("https://");
}

auto take_value(std::span<const std::string> arguments, size_t& index, std::string_view option, std::string& error)
    -> std::string {
  if (index + 1 >= arguments.size()) {
    error = std::format("missing value for {}", option);
    return {};
  }

  ++index;
  return arguments[index];
}

}  // namespace

auto parse_cli_args(std::span<const std::string> arguments, std::string& error) -> CliArgs {
  CliArgs args;

  for (size_t i = 1; i < arguments.size(); ++i) {
    const auto& argument = arguments[i];

    if (argument == "--help" || argument == "-h") {
      args.show_help = true;
      return args;
    }
    if (argument == "--version" || argument == "-v") {
      args.show_version = true;
      return args;
    }
    if (argument == "--preset" || argument == "-p") {
      args.preset_source = take_value(arguments, i, argument, error);
      if (!error.empty()) {
        return {};
      }
      continue;
    }
    if (argument == "--convert-autoeq") {
      args.convert_autoeq_source = take_value(arguments, i, argument, error);
      if (!error.empty()) {
        return {};
      }
      continue;
    }
    if (argument == "--output" || argument == "-o") {
      args.output_path = take_value(arguments, i, argument, error);
      if (!error.empty()) {
        return {};
      }
      continue;
    }
    if (argument == "--sink" || argument == "-s") {
      args.sink_selector = take_value(arguments, i, argument, error);
      if (!error.empty()) {
        return {};
      }
      continue;
    }
    if (argument == "--dry-run" || argument == "-d") {
      args.dry_run = true;
      continue;
    }
    if (argument == "--list-sinks") {
      args.list_sinks = true;
      continue;
    }

    error = std::format("unknown option: {}", argument);
    return {};
  }

  if (!args.preset_source.empty() && !args.convert_autoeq_source.empty()) {
    error = "--preset and --convert-autoeq cannot be used together";
    return {};
  }

  if (!args.output_path.empty() && args.convert_autoeq_source.empty()) {
    error = "--output requires --convert-autoeq";
    return {};
  }

  if (!args.convert_autoeq_source.empty()) {
    if (args.list_sinks) {
      error = "--convert-autoeq cannot be used with --list-sinks";
      return {};
    }
    if (!args.sink_selector.empty()) {
      error = "--convert-autoeq cannot be used with --sink";
      return {};
    }
    if (args.dry_run) {
      error = "--convert-autoeq cannot be used with --dry-run";
      return {};
    }
    if (is_url(args.convert_autoeq_source)) {
      error = "--convert-autoeq must be a local file path";
      return {};
    }
    return args;
  }

  if (args.preset_source.empty() && !args.list_sinks) {
    if (const char* default_preset = std::getenv(kDefaultPresetEnv); default_preset != nullptr && *default_preset != '\0') {
      args.preset_source = default_preset;
      args.preset_from_env = true;
    }
  }

  if (args.preset_source.empty() && !args.list_sinks) {
    error = std::format("no preset specified; use --preset <path> or set {}", kDefaultPresetEnv);
    return {};
  }
  if (is_url(args.preset_source)) {
    error = "--preset must be a local file path; URL presets are no longer supported";
    return {};
  }

  return args;
}

auto cli_help_text(std::string_view executable_name) -> std::string {
  return std::format(
      "Usage: {} [options]\n"
      "Load an EasyEffects-compatible EQ preset into a minimal headless PipeWire/LV2 runtime.\n\n"
      "Daemon:\n"
      "      {} daemon start [--preset <path>] [--sink <name>]\n"
      "      {} status\n"
      "      {} doctor\n"
      "      {} health\n"
      "      {} current-sink\n"
      "      {} apply <preset> [--sink <name>]\n"
      "      {} enable\n"
      "      {} disable\n"
      "      {} bypass on|off\n"
      "      {} volume <0.0-1.5>\n"
      "      {} list-sinks\n"
      "      {} switch-sink <name-or-serial>\n"
      "      {} shutdown\n\n"
      "Standalone:\n"
      "  -p, --preset <preset>  Local EasyEffects EQ preset path (one-shot mode).\n"
      "      --convert-autoeq <text>\n"
      "                        Convert a local AutoEQ-style parametric EQ text file into EasyEffects JSON.\n"
      "  -o, --output <path>    Write converted JSON to a file instead of stdout.\n"
      "  -s, --sink <sink>      Optional sink override by PipeWire node name or object serial.\n"
      "  -d, --dry-run          Validate preset, resolve plugins/kernels, print what would be applied, and exit.\n"
      "      --list-sinks       List current PipeWire output sinks and exit.\n"
      "  -h, --help             Display this help and exit.\n"
      "  -v, --version          Display version information and exit.\n"
      "\n"
      "Environment:\n"
      "      {}  Fallback local preset path when --preset is omitted\n"
      "          (standalone mode and daemon start bootstrap only).\n",
      executable_name,
      executable_name,
      executable_name,
      executable_name,
      executable_name,
      executable_name,
      executable_name,
      executable_name,
      executable_name,
      executable_name,
      executable_name,
      executable_name,
      executable_name,
      executable_name,
      kDefaultPresetEnv);
}

}  // namespace ee
