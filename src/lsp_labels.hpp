#pragma once

#include <algorithm>
#include <array>
#include <ranges>
#include <string_view>

namespace ee::labels {

constexpr std::array kEqModeLabels = {"IIR", "FIR", "FFT", "SPM"};
constexpr std::array kBandModeLabels = {"RLC (BT)", "RLC (MT)", "BWC (BT)", "BWC (MT)",
                                        "LRX (BT)", "LRX (MT)", "APO (DR)"};
constexpr std::array kBandTypeLabels = {"Off",        "Bell",       "Hi-pass",    "Hi-shelf",
                                        "Lo-pass",    "Lo-shelf",   "Notch",      "Resonance",
                                        "Allpass",    "Bandpass",   "Ladder-pass","Ladder-rej"};
constexpr std::array kBandSlopeLabels = {"x1", "x2", "x3", "x4"};

constexpr std::array kLimiterModeLabels = {"Herm Thin", "Herm Wide", "Herm Tail", "Herm Duck",
                                           "Exp Thin",  "Exp Wide",  "Exp Tail",  "Exp Duck",
                                           "Line Thin", "Line Wide", "Line Tail", "Line Duck"};
constexpr std::array kLimiterOversamplingLabels = {
    "None",          "Half x2/16 bit", "Half x2/24 bit", "Half x3/16 bit", "Half x3/24 bit", "Half x4/16 bit",
    "Half x4/24 bit","Half x6/16 bit", "Half x6/24 bit", "Half x8/16 bit", "Half x8/24 bit", "Full x2/16 bit",
    "Full x2/24 bit","Full x3/16 bit", "Full x3/24 bit", "Full x4/16 bit", "Full x4/24 bit", "Full x6/16 bit",
    "Full x6/24 bit","Full x8/16 bit", "Full x8/24 bit", "True Peak/16 bit","True Peak/24 bit"};
constexpr std::array kLimiterDitheringLabels = {"None", "7bit", "8bit", "11bit", "12bit",
                                                "15bit", "16bit", "23bit", "24bit"};
constexpr std::array kLimiterSidechainLabels = {"Internal", "External", "Link"};

template <typename Container>
inline auto label_index(const Container& labels, std::string_view value) -> float {
  const auto it = std::ranges::find(labels, value);
  return static_cast<float>(it == labels.end() ? 0 : std::distance(labels.begin(), it));
}

}  // namespace ee::labels
