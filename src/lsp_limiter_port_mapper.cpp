#include "lsp_limiter_port_mapper.hpp"

#include "lsp_labels.hpp"
#include "math_utils.hpp"

namespace ee {

using namespace ee::labels;

namespace {

auto db_port_value(const double db, const bool enforce_lower_bound) -> float {
  if (enforce_lower_bound && db <= -80.01) {
    return 0.0F;
  }
  return static_cast<float>(math::db_to_linear(db));
}

}  // namespace

void apply_limiter_preset_to_host(const LimiterPreset& preset, Lv2HostCore& host) {
  host.set_control_port_value("mode", label_index(kLimiterModeLabels, preset.mode));
  host.set_control_port_value("ovs", label_index(kLimiterOversamplingLabels, preset.oversampling));
  host.set_control_port_value("dith", label_index(kLimiterDitheringLabels, preset.dithering));
  host.set_control_port_value("extsc", label_index(kLimiterSidechainLabels, preset.sidechain_type));
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
