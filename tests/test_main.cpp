#include <array>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sndfile.hh>
#include <string>
#include <vector>
#include <string_view>

#include "cli_args.hpp"
#include "convolver_host.hpp"
#include "ee_eq_preset_parser.hpp"
#include "kernel_resolver.hpp"
#include "preset_source.hpp"

namespace {

int g_failures = 0;

struct TempDir {
  std::filesystem::path path;

  ~TempDir() {
    if (!path.empty()) {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  }

  auto is_valid() const -> bool {
    return !path.empty();
  }
};

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++g_failures;
  }
}

auto fixture_path(std::string_view file_name) -> std::string {
  return std::string(EE_EQ_CLI_TEST_FIXTURE_DIR) + "/" + std::string(file_name);
}

auto make_temp_dir() -> TempDir {
  std::array<char, 64> pattern{};
  std::snprintf(pattern.data(), pattern.size(), "/tmp/ee-eq-cli-tests-XXXXXX");
  if (char* created = mkdtemp(pattern.data()); created != nullptr) {
    return TempDir{.path = created};
  }
  return {};
}

auto resolver_ir_dir(const TempDir& temp_dir) -> std::filesystem::path {
  setenv("XDG_DATA_HOME", temp_dir.path.c_str(), 1);
  return temp_dir.path / "ee-eq-cli" / "irs";
}

void test_cli_args_accept_local_preset() {
  std::string error;
  const std::vector<std::string> arguments = {"ee-eq-cli", "--preset", fixture_path("Boosted.json")};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "local preset path should parse");
  expect(args.preset_source == fixture_path("Boosted.json"), "parsed preset path should be preserved");
  expect(!args.preset_from_env, "direct preset path should not be marked as env-derived");
  expect(!args.list_sinks, "--list-sinks should default to false");
}

void test_cli_args_reject_url_preset() {
  std::string error;
  const std::vector<std::string> arguments = {"ee-eq-cli", "--preset", "https://example.invalid/preset.json"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(args.preset_source.empty(), "URL preset should not be accepted");
  expect(error == "--preset must be a local file path; URL presets are no longer supported",
         "URL preset should produce the local-file-only diagnostic");
}

void test_cli_args_allow_list_sinks_without_preset() {
  std::string error;
  const std::vector<std::string> arguments = {"ee-eq-cli", "--list-sinks"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "--list-sinks should not require --preset");
  expect(args.list_sinks, "--list-sinks should be parsed");
}

void test_cli_args_require_preset_or_env() {
  unsetenv("EE_EQ_CLI_DEFAULT_PRESET");

  std::string error;
  const std::vector<std::string> arguments = {"ee-eq-cli"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(args.preset_source.empty(), "missing preset should not produce a preset path");
  expect(error == "no preset specified; use --preset <path> or set EE_EQ_CLI_DEFAULT_PRESET",
         "missing preset should produce the actionable error");
}

void test_cli_args_use_default_preset_env() {
  setenv("EE_EQ_CLI_DEFAULT_PRESET", fixture_path("Boosted.json").c_str(), 1);

  std::string error;
  const std::vector<std::string> arguments = {"ee-eq-cli"};
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "default preset env should satisfy the preset requirement");
  expect(args.preset_source == fixture_path("Boosted.json"), "default preset env should populate the preset path");
  expect(args.preset_from_env, "default preset env should be marked as env-derived");

  unsetenv("EE_EQ_CLI_DEFAULT_PRESET");
}

void test_cli_args_cli_wins_over_default_preset_env() {
  setenv("EE_EQ_CLI_DEFAULT_PRESET", fixture_path("Boosted.json").c_str(), 1);

  std::string error;
  const std::vector<std::string> arguments = {
      "ee-eq-cli",
      "--preset",
      fixture_path("Bass Enhancing + Perfect EQ - Low Latency.json"),
  };
  const auto args = ee::parse_cli_args(arguments, error);
  expect(error.empty(), "cli preset should still parse with default preset env set");
  expect(args.preset_source == fixture_path("Bass Enhancing + Perfect EQ - Low Latency.json"),
         "cli preset should override the default preset env");
  expect(!args.preset_from_env, "cli preset should clear the env-derived marker");

  unsetenv("EE_EQ_CLI_DEFAULT_PRESET");
}

void test_load_preset_local_file() {
  std::string error;
  const auto loaded = ee::load_preset_source(fixture_path("Boosted.json"), error);
  expect(error.empty(), "fixture preset should load");
  expect(!loaded.bytes.empty(), "fixture preset bytes should be present");
  expect(loaded.origin.ends_with("Boosted.json"), "loaded origin should point at the fixture");
}

void test_load_preset_missing_file() {
  std::string error;
  const auto loaded = ee::load_preset_source(fixture_path("missing-preset.json"), error);
  expect(loaded.bytes.empty(), "missing preset should not load bytes");
  expect(error == "failed to open preset file", "missing preset should report a file-open error");
}

void test_load_preset_reject_url() {
  std::string error;
  const auto loaded = ee::load_preset_source("https://example.invalid/preset.json", error);
  expect(loaded.bytes.empty(), "URL preset should not load bytes");
  expect(error == "URL presets are no longer supported; use a local preset file path",
         "loader should reject URL presets explicitly");
}

void test_parse_fixture_preset() {
  std::string error;
  const auto loaded = ee::load_preset_source(fixture_path("Boosted.json"), error);
  expect(error.empty(), "fixture preset load for parsing should succeed");

  std::string parse_error;
  const auto parsed = ee::parse_easy_effects_preset(loaded.bytes, parse_error);
  expect(parse_error.empty(), "fixture preset should parse");
  expect(parsed.plugin_order.size() == 1, "fixture preset should contain a single supported plugin");
  expect(!parsed.plugin_order.empty() && parsed.plugin_order.front() == "equalizer",
         "fixture preset should preserve equalizer ordering");
  expect(parsed.equalizer.mode == "IIR", "fixture equalizer mode should parse");
}

void test_parse_fixture_with_convolver_and_limiter() {
  std::string error;
  const auto loaded =
      ee::load_preset_source(fixture_path("Bass Enhancing + Perfect EQ - Low Latency.json"), error);
  expect(error.empty(), "convolver/limiter preset fixture should load");

  std::string parse_error;
  const auto parsed = ee::parse_easy_effects_preset(loaded.bytes, parse_error);
  expect(parse_error.empty(), "convolver/limiter preset fixture should parse");
  expect(parsed.plugin_order.size() == 3, "convolver/limiter fixture should keep three supported plugins");
  expect(parsed.plugin_order == std::vector<std::string>({"equalizer", "convolver", "limiter"}),
         "convolver/limiter fixture should preserve normalized plugin ordering");
  expect(parsed.convolver.has_value(), "convolver fixture should produce a convolver payload");
  expect(parsed.limiter.has_value(), "convolver fixture should produce a limiter payload");
}

void test_parse_invalid_preset() {
  std::string error;
  const auto loaded = ee::load_preset_source(fixture_path("invalid-preset.json"), error);
  expect(error.empty(), "invalid preset fixture should still load as a file");

  std::string parse_error;
  const auto parsed = ee::parse_easy_effects_preset(loaded.bytes, parse_error);
  expect(parse_error.starts_with("invalid preset JSON:"), "invalid preset should produce a JSON parse error");
  expect(parsed.plugin_order.empty(), "invalid preset should not produce plugin ordering");
}

void test_parse_duplicate_plugin_kind_rejected() {
  constexpr std::string_view duplicate_preset = R"({
    "output": {
      "plugins_order": ["equalizer", "equalizer#0"],
      "equalizer": {
        "bypass": false,
        "input-gain": 0.0,
        "output-gain": 0.0,
        "mode": "IIR",
        "num-bands": 2,
        "split-channels": false,
        "left": {},
        "right": {}
      },
      "equalizer#0": {
        "bypass": false,
        "input-gain": 0.0,
        "output-gain": 0.0,
        "mode": "IIR",
        "num-bands": 2,
        "split-channels": false,
        "left": {},
        "right": {}
      }
    }
  })";

  std::string error;
  const auto parsed = ee::parse_easy_effects_preset(duplicate_preset, error);
  expect(error == "duplicate plugin kind is not supported: equalizer",
         "duplicate normalized plugin kinds should be rejected explicitly");
  expect(parsed.plugin_order.empty(), "duplicate plugin kinds should not produce a parsed plugin order");
}

void test_resolve_kernel_exact_match() {
  const auto temp_dir = make_temp_dir();
  expect(temp_dir.is_valid(), "temporary directory for kernel resolution should be created");
  if (!temp_dir.is_valid()) {
    return;
  }

  const auto ir_dir = resolver_ir_dir(temp_dir);
  expect(std::filesystem::create_directories(ir_dir), "IR directory should be created");

  std::ofstream file(ir_dir / "room.irs", std::ios::binary);
  expect(file.is_open(), "exact-match IR file should be writable");
  file << "stub";
  file.close();

  ee::ConvolverPreset preset;
  preset.kernel_name = "room";
  std::string warning;
  const auto resolved = ee::resolve_convolver_kernel(preset, warning);
  expect(resolved.has_value(), "exact kernel match should resolve");
  expect(warning.empty(), "exact kernel match should not warn");
  expect(resolved.has_value() && resolved->name == "room", "exact kernel match should preserve the kernel name");
}

void test_resolve_kernel_fuzzy_match() {
  const auto temp_dir = make_temp_dir();
  expect(temp_dir.is_valid(), "temporary directory for fuzzy kernel resolution should be created");
  if (!temp_dir.is_valid()) {
    return;
  }

  const auto ir_dir = resolver_ir_dir(temp_dir);
  expect(std::filesystem::create_directories(ir_dir), "IR directory should exist for fuzzy match");

  std::ofstream file(ir_dir / "Room Correction.irs", std::ios::binary);
  expect(file.is_open(), "fuzzy-match IR file should be writable");
  file << "stub";
  file.close();

  ee::ConvolverPreset preset;
  preset.kernel_name = "room_correction";
  std::string warning;
  const auto resolved = ee::resolve_convolver_kernel(preset, warning);
  expect(resolved.has_value(), "fuzzy kernel match should resolve");
  expect(warning.find("fuzzy local match") != std::string::npos, "fuzzy kernel match should emit a warning");
  expect(resolved.has_value() && resolved->name == "Room Correction",
         "fuzzy kernel match should return the matched local file name");
}

void test_resolve_kernel_missing() {
  const auto temp_dir = make_temp_dir();
  expect(temp_dir.is_valid(), "temporary directory for missing kernel resolution should be created");
  if (!temp_dir.is_valid()) {
    return;
  }

  setenv("XDG_DATA_HOME", temp_dir.path.c_str(), 1);

  ee::ConvolverPreset preset;
  preset.kernel_name = "does-not-exist";
  std::string warning;
  const auto resolved = ee::resolve_convolver_kernel(preset, warning);
  expect(!resolved.has_value(), "missing kernel should not resolve");
  expect(warning.find("convolver kernel not found") != std::string::npos,
         "missing kernel should emit a not-found warning");
}

void test_convolver_validate_rate() {
  const auto temp_dir = make_temp_dir();
  expect(temp_dir.is_valid(), "temporary directory for convolver rate validation should be created");
  if (!temp_dir.is_valid()) {
    return;
  }

  const auto ir_dir = resolver_ir_dir(temp_dir);
  expect(std::filesystem::create_directories(ir_dir), "IR directory should be created for convolver rate validation");

  const auto ir_path = ir_dir / "rate-check.irs";
  {
    SndfileHandle sndfile(ir_path.string(), SFM_WRITE, SF_FORMAT_WAV | SF_FORMAT_PCM_16, 2, 44100);
    expect(sndfile.error() == 0, "rate-check fixture should be writable as audio");
    const std::array<float, 4> frames = {1.0F, 1.0F, 0.0F, 0.0F};
    expect(sndfile.writef(frames.data(), 2) == 2, "rate-check audio fixture should be written");
  }

  ee::ResolvedKernel kernel{
      .name = "rate-check",
      .path = ir_path.string(),
      .is_sofa = false,
  };

  ee::ConvolverPreset preset;
  ee::ConvolverHost host;
  std::string error;
  const auto loaded = host.load(preset, kernel, error);
  expect(loaded, "valid IR fixture should load into the convolver host");
  expect(error.empty(), "valid IR fixture should load without error");

  error.clear();
  expect(!host.validate_rate(48000, error), "mismatched convolver rate should be rejected");
  expect(error == "convolver kernel sample rate 44100 Hz does not match active stream rate 48000 Hz",
         "mismatched convolver rate should produce a clear error");

  error.clear();
  expect(host.validate_rate(44100, error), "matching convolver rate should be accepted");
  expect(error.empty(), "matching convolver rate should not warn");
}

}  // namespace

int main() {
  test_cli_args_accept_local_preset();
  test_cli_args_reject_url_preset();
  test_cli_args_allow_list_sinks_without_preset();
  test_cli_args_require_preset_or_env();
  test_cli_args_use_default_preset_env();
  test_cli_args_cli_wins_over_default_preset_env();
  test_load_preset_local_file();
  test_load_preset_missing_file();
  test_load_preset_reject_url();
  test_parse_fixture_preset();
  test_parse_fixture_with_convolver_and_limiter();
  test_parse_invalid_preset();
  test_parse_duplicate_plugin_kind_rejected();
  test_resolve_kernel_exact_match();
  test_resolve_kernel_fuzzy_match();
  test_resolve_kernel_missing();
  test_convolver_validate_rate();

  if (g_failures != 0) {
    std::cerr << g_failures << " test assertion(s) failed\n";
    return 1;
  }

  std::cout << "ee-eq-cli tests passed\n";
  return 0;
}
