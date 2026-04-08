#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ee {

struct EqBand {
  std::string type;
  std::string mode;
  std::string slope;
  bool solo = false;
  bool mute = false;
  double gain_db = 0.0;
  double frequency = 0.0;
  double q = 0.0;
  double width = 4.0;
};

struct EqPreset {
  bool bypass = false;
  double input_gain_db = 0.0;
  double output_gain_db = 0.0;
  std::string mode = "IIR";
  bool split_channels = false;
  double balance = 0.0;
  double pitch_left = 0.0;
  double pitch_right = 0.0;
  int num_bands = 32;
  std::array<EqBand, 32> left{};
  std::array<EqBand, 32> right{};
};

struct LimiterPreset {
  std::string mode = "Herm Thin";
  std::string oversampling = "None";
  std::string dithering = "None";
  std::string sidechain_type = "Internal";
  bool bypass = false;
  double input_gain_db = 0.0;
  double output_gain_db = 0.0;
  double lookahead = 5.0;
  double attack = 5.0;
  double release = 5.0;
  double threshold_db = 0.0;
  double sidechain_preamp_db = 0.0;
  double stereo_link = 100.0;
  bool alr = false;
  double alr_attack = 5.0;
  double alr_release = 50.0;
  double alr_knee_db = 0.0;
  double alr_knee_smooth_db = -5.0;
  bool gain_boost = true;
  double input_to_sidechain_db = -80.01;
  double input_to_link_db = -80.01;
  double sidechain_to_input_db = -80.01;
  double sidechain_to_link_db = -80.01;
  double link_to_input_db = -80.01;
  double link_to_sidechain_db = -80.01;
};

struct ConvolverPreset {
  bool bypass = false;
  double input_gain_db = 0.0;
  double output_gain_db = 0.0;
  std::string kernel_name;
  std::string kernel_path;
  int ir_width = 100;
  bool autogain = false;
  double dry_db = -100.0;
  double wet_db = 0.0;
  double sofa_azimuth = 0.0;
  double sofa_elevation = 0.0;
  double sofa_radius = 1.0;
};

struct ParsedPreset {
  std::vector<std::string> plugin_order;
  std::vector<std::string> warnings;
  EqPreset equalizer;
  std::optional<LimiterPreset> limiter;
  std::optional<ConvolverPreset> convolver;
};

auto parse_easy_effects_preset(std::string_view bytes, std::string& error) -> ParsedPreset;

}  // namespace ee
