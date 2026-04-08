#include "lsp_limiter_port_mapper.hpp"

#include <ranges>
#include <vector>

#include "math_utils.hpp"

namespace ee {

namespace {

const std::vector<std::string> kModeLabels = {"Herm Thin", "Herm Wide", "Herm Tail", "Herm Duck",
                                              "Exp Thin",  "Exp Wide",  "Exp Tail",  "Exp Duck",
                                              "Line Thin", "Line Wide", "Line Tail", "Line Duck"};
const std::vector<std::string> kOversamplingLabels = {
    "None",          "Half x2/16 bit", "Half x2/24 bit", "Half x3/16 bit", "Half x3/24 bit", "Half x4/16 bit",
    "Half x4/24 bit","Half x6/16 bit", "Half x6/24 bit", "Half x8/16 bit", "Half x8/24 bit", "Full x2/16 bit",
    "Full x2/24 bit","Full x3/16 bit", "Full x3/24 bit", "Full x4/16 bit", "Full x4/24 bit", "Full x6/16 bit",
    "Full x6/24 bit","Full x8/16 bit", "Full x8/24 bit", "True Peak/16 bit","True Peak/24 bit"};
const std::vector<std::string> kDitheringLabels = {"None", "7bit", "8bit", "11bit", "12bit", "15bit", "16bit",
                                                   "23bit", "24bit"};
const std::vector<std::string> kSidechainTypeLabels = {"Internal", "External", "Link"};

auto label_index(const std::vector<std::string>& labels, const std::string& value) -> float {
  const auto it = std::ranges::find(labels, value);
  return static_cast<float>(it == labels.end() ? 0 : std::distance(labels.begin(), it));
}

auto db_port_value(const double db, const bool enforce_lower_bound) -> float {
  if (enforce_lower_bound && db <= -80.01) {
    return 0.0F;
  }
  return static_cast<float>(math::db_to_linear(db));
}

}  // namespace

void apply_limiter_preset_to_host(const LimiterPreset& preset, Lv2HostCore& host) {
  host.set_control_port_value("mode", label_index(kModeLabels, preset.mode));
  host.set_control_port_value("ovs", label_index(kOversamplingLabels, preset.oversampling));
  host.set_control_port_value("dith", label_index(kDitheringLabels, preset.dithering));
  host.set_control_port_value("extsc", label_index(kSidechainTypeLabels, preset.sidechain_type));
  host.set_control_port_value("lk", static_cast<float>(preset.lookahead));
  host.set_control_port_value("at", static_cast<float>(preset.attack));
  host.set_control_port_value("rt", static_cast<float>(preset.release));
  host.set_control_port_value("boost", preset.gain_boost ? 1.0F : 0.0F);
  host.set_control_port_value("slink", static_cast<float>(preset.stereo_link));
  host.set_control_port_value("alr", preset.alr ? 1.0F : 0.0F);
  host.set_control_port_value("alr_at", static_cast<float>(preset.alr_attack));
  host.set_control_port_value("alr_rt", static_cast<float>(preset.alr_release));
  host.set_control_port_value("smooth", static_cast<float>(preset.alr_knee_smooth_db));
  host.set_control_port_value("th", db_port_value(preset.threshold_db, false));
  host.set_control_port_value("knee", db_port_value(preset.alr_knee_db, false));
  host.set_control_port_value("scp", db_port_value(preset.sidechain_preamp_db, true));
  host.set_control_port_value("in2lk", db_port_value(preset.input_to_link_db, true));
  host.set_control_port_value("in2sc", db_port_value(preset.input_to_sidechain_db, true));
  host.set_control_port_value("sc2in", db_port_value(preset.sidechain_to_input_db, true));
  host.set_control_port_value("sc2lk", db_port_value(preset.sidechain_to_link_db, true));
  host.set_control_port_value("lk2in", db_port_value(preset.link_to_input_db, true));
  host.set_control_port_value("lk2sc", db_port_value(preset.link_to_sidechain_db, true));
}

}  // namespace ee
