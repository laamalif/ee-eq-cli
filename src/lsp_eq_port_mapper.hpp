#pragma once

#include "ee_eq_preset_parser.hpp"
#include "lv2_host_core.hpp"

namespace ee {

void apply_eq_preset_to_host(const EqPreset& preset, Lv2HostCore& host);

}  // namespace ee
