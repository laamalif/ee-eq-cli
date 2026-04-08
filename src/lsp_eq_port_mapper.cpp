#include "lsp_eq_port_mapper.hpp"

#include <ranges>
#include <vector>

#include "math_utils.hpp"
#include "tags_equalizer.hpp"

namespace ee {

namespace {

auto label_index(const std::vector<std::string>& labels, const std::string& value) -> float {
  const auto it = std::ranges::find(labels, value);
  return static_cast<float>(it == labels.end() ? 0 : std::distance(labels.begin(), it));
}

const std::vector<std::string> kModeLabels = {"IIR", "FIR", "FFT", "SPM"};
const std::vector<std::string> kBandModeLabels = {"RLC (BT)", "RLC (MT)", "BWC (BT)", "BWC (MT)",
                                                  "LRX (BT)", "LRX (MT)", "APO (DR)"};
const std::vector<std::string> kBandTypeLabels = {"Off",        "Bell",       "Hi-pass",    "Hi-shelf",
                                                  "Lo-pass",    "Lo-shelf",   "Notch",      "Resonance",
                                                  "Allpass",    "Bandpass",   "Ladder-pass","Ladder-rej"};
const std::vector<std::string> kBandSlopeLabels = {"x1", "x2", "x3", "x4"};

void apply_channel(const std::array<EqBand, 32>& bands,
                   const auto& type_ports,
                   const auto& mode_ports,
                   const auto& slope_ports,
                   const auto& solo_ports,
                   const auto& mute_ports,
                   const auto& frequency_ports,
                   const auto& q_ports,
                   const auto& width_ports,
                   const auto& gain_ports,
                   Lv2HostCore& host) {
  for (size_t i = 0; i < bands.size(); ++i) {
    host.set_control_port_value(type_ports[i].data(), label_index(kBandTypeLabels, bands[i].type));
    host.set_control_port_value(mode_ports[i].data(), label_index(kBandModeLabels, bands[i].mode));
    host.set_control_port_value(slope_ports[i].data(), label_index(kBandSlopeLabels, bands[i].slope));
    host.set_control_port_value(solo_ports[i].data(), bands[i].solo ? 1.0F : 0.0F);
    host.set_control_port_value(mute_ports[i].data(), bands[i].mute ? 1.0F : 0.0F);
    host.set_control_port_value(frequency_ports[i].data(), static_cast<float>(bands[i].frequency));
    host.set_control_port_value(q_ports[i].data(), static_cast<float>(bands[i].q));
    host.set_control_port_value(width_ports[i].data(), static_cast<float>(bands[i].width));
    host.set_control_port_value(gain_ports[i].data(), static_cast<float>(math::db_to_linear(bands[i].gain_db)));
  }
}

}  // namespace

void apply_eq_preset_to_host(const EqPreset& preset, Lv2HostCore& host) {
  host.set_control_port_value("mode", label_index(kModeLabels, preset.mode));
  host.set_control_port_value("bal", static_cast<float>(preset.balance));
  host.set_control_port_value("frqs_l", static_cast<float>(preset.pitch_left));
  host.set_control_port_value("frqs_r", static_cast<float>(preset.pitch_right));
  host.set_control_port_value("clink", preset.split_channels ? 0.0F : 1.0F);

  apply_channel(preset.left, tags::equalizer::ftl, tags::equalizer::fml, tags::equalizer::sl, tags::equalizer::xsl,
                tags::equalizer::xml, tags::equalizer::fl, tags::equalizer::ql, tags::equalizer::wl,
                tags::equalizer::gl, host);

  apply_channel(preset.right, tags::equalizer::ftr, tags::equalizer::fmr, tags::equalizer::sr, tags::equalizer::xsr,
                tags::equalizer::xmr, tags::equalizer::fr, tags::equalizer::qr, tags::equalizer::wr,
                tags::equalizer::gr, host);
}

}  // namespace ee
