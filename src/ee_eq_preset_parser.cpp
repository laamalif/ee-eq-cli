#include "ee_eq_preset_parser.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "lsp_labels.hpp"

namespace ee {

using namespace ee::labels;

namespace {

constexpr int kMaxBands = 32;

auto default_band_frequencies(const int num_bands) -> std::array<double, kMaxBands> {
  std::array<double, kMaxBands> values{};
  constexpr double min_freq = 20.0;
  constexpr double max_freq = 20000.0;

  double freq0 = min_freq;
  const double step = std::pow(max_freq / min_freq, 1.0 / static_cast<double>(num_bands));
  for (int i = 0; i < num_bands; ++i) {
    const double freq1 = freq0 * step;
    values[i] = freq0 + (0.5 * (freq1 - freq0));
    freq0 = freq1;
  }

  for (int i = num_bands; i < kMaxBands; ++i) {
    values[i] = values[std::max(0, num_bands - 1)];
  }

  return values;
}

auto default_band_qs(const int num_bands) -> std::array<double, kMaxBands> {
  std::array<double, kMaxBands> values{};
  constexpr double min_freq = 20.0;
  constexpr double max_freq = 20000.0;

  double freq0 = min_freq;
  const double step = std::pow(max_freq / min_freq, 1.0 / static_cast<double>(num_bands));
  for (int i = 0; i < num_bands; ++i) {
    const double freq1 = freq0 * step;
    const double freq = freq0 + (0.5 * (freq1 - freq0));
    const double width = freq1 - freq0;
    values[i] = freq / width;
    freq0 = freq1;
  }

  for (int i = num_bands; i < kMaxBands; ++i) {
    values[i] = values[std::max(0, num_bands - 1)];
  }

  return values;
}

template <typename Container>
auto validate_label(const std::string& value,
                    const Container& labels,
                    std::string_view field,
                    std::string& error)
    -> std::string {
  if (std::ranges::find(labels, value) == labels.end()) {
    error = std::format("unsupported value '{}' for {}", value, field);
    return {};
  }
  return value;
}

auto band_default() -> EqBand {
  return EqBand{
      .type = "Bell",
      .mode = "RLC (BT)",
      .slope = "x1",
      .solo = false,
      .mute = false,
      .gain_db = 0.0,
      .frequency = 0.0,
      .q = 4.36,
      .width = 4.0,
  };
}

auto parse_band(const nlohmann::json& json,
                const std::string& key,
                const EqBand& defaults,
                std::string& error) -> EqBand {
  EqBand band = defaults;

  if (!json.contains(key)) {
    return band;
  }

  const auto& value = json.at(key);
  band.type = validate_label(value.value("type", band.type), kBandTypeLabels, key + ".type", error);
  if (!error.empty()) {
    return {};
  }

  band.mode = validate_label(value.value("mode", band.mode), kBandModeLabels, key + ".mode", error);
  if (!error.empty()) {
    return {};
  }

  band.slope = validate_label(value.value("slope", band.slope), kBandSlopeLabels, key + ".slope", error);
  if (!error.empty()) {
    return {};
  }

  band.solo = value.value("solo", band.solo);
  band.mute = value.value("mute", band.mute);
  band.gain_db = value.value("gain", band.gain_db);
  band.frequency = value.value("frequency", band.frequency);
  band.q = value.value("q", band.q);
  band.width = value.value("width", band.width);

  return band;
}

auto find_eq_instance_key(const nlohmann::json& output, std::string& error) -> std::string {
  if (output.contains("equalizer")) {
    return "equalizer";
  }

  for (auto it = output.begin(); it != output.end(); ++it) {
    if (it.key().rfind("equalizer#", 0) == 0) {
      return it.key();
    }
  }

  error = "missing output.equalizer payload";
  return {};
}

auto find_instance_key(const nlohmann::json& output, const std::string& base_name) -> std::optional<std::string> {
  if (output.contains(base_name)) {
    return base_name;
  }

  for (auto it = output.begin(); it != output.end(); ++it) {
    if (it.key().rfind(base_name + "#", 0) == 0) {
      return it.key();
    }
  }

  return std::nullopt;
}

auto push_plugin_kind(std::vector<std::string>& plugin_order,
                      std::string_view plugin_kind,
                      std::string& error) -> bool {
  const auto duplicate = std::ranges::find(plugin_order, plugin_kind) != plugin_order.end();
  if (duplicate) {
    error = std::format("duplicate plugin kind is not supported: {}", plugin_kind);
    return false;
  }

  plugin_order.emplace_back(plugin_kind);
  return true;
}

auto parse_limiter(const nlohmann::json& output, const std::string& key, std::string& error) -> LimiterPreset {
  LimiterPreset preset;
  const auto& json = output.at(key);

  preset.mode = validate_label(json.value("mode", preset.mode), kLimiterModeLabels, "limiter.mode", error);
  if (!error.empty()) return {};
  preset.oversampling =
      validate_label(json.value("oversampling", preset.oversampling), kLimiterOversamplingLabels, "limiter.oversampling", error);
  if (!error.empty()) return {};
  preset.dithering =
      validate_label(json.value("dithering", preset.dithering), kLimiterDitheringLabels, "limiter.dithering", error);
  if (!error.empty()) return {};
  preset.sidechain_type =
      validate_label(json.value("sidechain-type", preset.sidechain_type), kLimiterSidechainLabels, "limiter.sidechain-type", error);
  if (!error.empty()) return {};

  preset.bypass = json.value("bypass", preset.bypass);
  preset.input_gain_db = json.value("input-gain", preset.input_gain_db);
  preset.output_gain_db = json.value("output-gain", preset.output_gain_db);
  preset.lookahead = json.value("lookahead", preset.lookahead);
  preset.attack = json.value("attack", preset.attack);
  preset.release = json.value("release", preset.release);
  preset.threshold_db = json.value("threshold", preset.threshold_db);
  preset.sidechain_preamp_db = json.value("sidechain-preamp", preset.sidechain_preamp_db);
  preset.stereo_link = json.value("stereo-link", preset.stereo_link);
  preset.alr = json.value("alr", preset.alr);
  preset.alr_attack = json.value("alr-attack", preset.alr_attack);
  preset.alr_release = json.value("alr-release", preset.alr_release);
  preset.alr_knee_db = json.value("alr-knee", preset.alr_knee_db);
  preset.alr_knee_smooth_db = json.value("alr-knee-smooth", preset.alr_knee_smooth_db);
  preset.gain_boost = json.value("gain-boost", preset.gain_boost);
  preset.input_to_sidechain_db = json.value("input-to-sidechain", preset.input_to_sidechain_db);
  preset.input_to_link_db = json.value("input-to-link", preset.input_to_link_db);
  preset.sidechain_to_input_db = json.value("sidechain-to-input", preset.sidechain_to_input_db);
  preset.sidechain_to_link_db = json.value("sidechain-to-link", preset.sidechain_to_link_db);
  preset.link_to_input_db = json.value("link-to-input", preset.link_to_input_db);
  preset.link_to_sidechain_db = json.value("link-to-sidechain", preset.link_to_sidechain_db);
  return preset;
}

auto parse_convolver(const nlohmann::json& output, const std::string& key) -> ConvolverPreset {
  ConvolverPreset preset;
  const auto& json = output.at(key);
  preset.bypass = json.value("bypass", preset.bypass);
  preset.input_gain_db = json.value("input-gain", preset.input_gain_db);
  preset.output_gain_db = json.value("output-gain", preset.output_gain_db);
  preset.kernel_name = json.value("kernel-name", preset.kernel_name);
  preset.kernel_path = json.value("kernel-path", preset.kernel_path);
  preset.ir_width = json.value("ir-width", preset.ir_width);
  preset.autogain = json.value("autogain", preset.autogain);
  preset.dry_db = json.value("dry", preset.dry_db);
  preset.wet_db = json.value("wet", preset.wet_db);
  if (json.contains("sofa") && json.at("sofa").is_object()) {
    const auto& sofa = json.at("sofa");
    preset.sofa_azimuth = sofa.value("azimuth", preset.sofa_azimuth);
    preset.sofa_elevation = sofa.value("elevation", preset.sofa_elevation);
    preset.sofa_radius = sofa.value("radius", preset.sofa_radius);
  }
  return preset;
}

}  // namespace

auto parse_easy_effects_preset(std::string_view bytes, std::string& error) -> ParsedPreset {
  ParsedPreset parsed;
  EqPreset& preset = parsed.equalizer;

  nlohmann::json json;
  try {
    json = nlohmann::json::parse(bytes.begin(), bytes.end());
  } catch (const std::exception& e) {
    error = std::format("invalid preset JSON: {}", e.what());
    return {};
  }

  if (!json.contains("output") || !json.at("output").is_object()) {
    error = "missing output section";
    return {};
  }

  const auto& output = json.at("output");
  if (!output.contains("plugins_order") || !output.at("plugins_order").is_array()) {
    error = "missing output.plugins_order";
    return {};
  }

  const auto plugins = output.at("plugins_order").get<std::vector<std::string>>();
  if (plugins.empty()) {
    error = "empty output.plugins_order is not supported";
    return {};
  }

  bool has_equalizer = false;
  for (const auto& plugin : plugins) {
    if (plugin.rfind("equalizer", 0) == 0) {
      has_equalizer = true;
      if (!push_plugin_kind(parsed.plugin_order, "equalizer", error)) {
        return {};
      }
    } else if (plugin.rfind("limiter", 0) == 0) {
      if (!push_plugin_kind(parsed.plugin_order, "limiter", error)) {
        return {};
      }
    } else if (plugin.rfind("convolver", 0) == 0) {
      if (!push_plugin_kind(parsed.plugin_order, "convolver", error)) {
        return {};
      }
    } else {
      parsed.warnings.push_back(std::format("skipping unsupported plugin: {}", plugin));
    }
  }
  if (!has_equalizer) {
    error = "equalizer is required in output.plugins_order";
    return {};
  }

  const auto instance_key = find_eq_instance_key(output, error);
  if (!error.empty()) {
    return {};
  }

  const auto& eq = output.at(instance_key);

  preset.bypass = eq.value("bypass", false);
  preset.input_gain_db = eq.value("input-gain", 0.0);
  preset.output_gain_db = eq.value("output-gain", 0.0);
  preset.mode = validate_label(eq.value("mode", preset.mode), kEqModeLabels, "mode", error);
  if (!error.empty()) {
    return {};
  }

  preset.num_bands = std::clamp(eq.value("num-bands", preset.num_bands), 1, kMaxBands);
  preset.split_channels = eq.value("split-channels", false);
  preset.balance = eq.value("balance", 0.0);
  preset.pitch_left = eq.value("pitch-left", 0.0);
  preset.pitch_right = eq.value("pitch-right", 0.0);

  const auto default_freq = default_band_frequencies(preset.num_bands);
  const auto default_q = default_band_qs(preset.num_bands);

  for (int i = 0; i < kMaxBands; ++i) {
    auto defaults = band_default();
    defaults.frequency = default_freq[i];
    defaults.q = default_q[i];
    if (i >= preset.num_bands) {
      defaults.type = "Off";
    }

    preset.left[i] = defaults;
    preset.right[i] = defaults;
  }

  if (!eq.contains("left") || !eq.at("left").is_object()) {
    error = "missing output.equalizer.left";
    return {};
  }

  if (!eq.contains("right") || !eq.at("right").is_object()) {
    error = "missing output.equalizer.right";
    return {};
  }

  for (int i = 0; i < kMaxBands; ++i) {
    const auto band_key = std::format("band{}", i);
    preset.left[i] = parse_band(eq.at("left"), band_key, preset.left[i], error);
    if (!error.empty()) {
      return {};
    }

    preset.right[i] = parse_band(eq.at("right"), band_key, preset.right[i], error);
    if (!error.empty()) {
      return {};
    }
  }

  if (!preset.split_channels) {
    preset.right = preset.left;
  }

  if (std::ranges::find(parsed.plugin_order, std::string("limiter")) != parsed.plugin_order.end()) {
    const auto limiter_key = find_instance_key(output, "limiter");
    if (!limiter_key.has_value()) {
      error = "missing output.limiter payload";
      return {};
    }

    parsed.limiter = parse_limiter(output, *limiter_key, error);
    if (!error.empty()) {
      return {};
    }
  }

  if (std::ranges::find(parsed.plugin_order, std::string("convolver")) != parsed.plugin_order.end()) {
    if (const auto convolver_key = find_instance_key(output, "convolver"); convolver_key.has_value()) {
      parsed.convolver = parse_convolver(output, *convolver_key);
    } else {
      parsed.warnings.push_back("convolver requested in plugins_order but no convolver payload was found");
      parsed.plugin_order.erase(std::remove(parsed.plugin_order.begin(), parsed.plugin_order.end(), "convolver"),
                                parsed.plugin_order.end());
    }
  }

  if (parsed.plugin_order.empty()) {
    error = "no supported plugin remained applicable after parsing";
    return {};
  }

  return parsed;
}

}  // namespace ee
