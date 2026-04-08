#include "preset_source.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace ee {

namespace {

auto load_local_file(const std::filesystem::path& path, std::string& error) -> LoadedPresetSource {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    error = "failed to open preset file";
    return {};
  }

  return {
      .origin = std::filesystem::absolute(path).string(),
      .bytes = std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()),
  };
}

}  // namespace

auto load_preset_source(std::string_view source, std::string& error) -> LoadedPresetSource {
  return load_local_file(std::filesystem::path(source), error);
}

}  // namespace ee
