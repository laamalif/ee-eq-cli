#pragma once

#include <string>

#include "ee_eq_preset_parser.hpp"

namespace ee {

struct ResolvedKernel {
  std::string name;
  std::string path;
  bool is_sofa = false;
};

auto resolve_convolver_kernel(const ConvolverPreset& preset, std::string& warning) -> std::optional<ResolvedKernel>;

}  // namespace ee
