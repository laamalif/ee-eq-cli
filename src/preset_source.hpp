#pragma once

#include <string>
#include <string_view>

namespace ee {

struct LoadedPresetSource {
  std::string origin;
  std::string bytes;
};

auto load_preset_source(std::string_view source, std::string& error) -> LoadedPresetSource;

}  // namespace ee
