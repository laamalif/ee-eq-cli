#include "kernel_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace ee {

namespace {

auto app_data_dir() -> std::filesystem::path {
  if (const char* xdg_data_home = std::getenv("XDG_DATA_HOME"); xdg_data_home != nullptr && *xdg_data_home != '\0') {
    return std::filesystem::path(xdg_data_home) / "ee-eq-cli";
  }

  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::filesystem::path(home) / ".local" / "share" / "ee-eq-cli";
  }

  return std::filesystem::current_path() / ".local" / "share" / "ee-eq-cli";
}

auto derive_kernel_name(const ConvolverPreset& preset) -> std::string {
  if (!preset.kernel_name.empty()) {
    return preset.kernel_name;
  }
  if (!preset.kernel_path.empty()) {
    return std::filesystem::path{preset.kernel_path}.stem().string();
  }
  return {};
}

auto lower_ascii(std::string value) -> std::string {
  std::ranges::transform(value, value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

auto normalize_name(std::string value) -> std::string {
  value = lower_ascii(std::move(value));

  std::string normalized;
  normalized.reserve(value.size());
  bool last_was_space = false;

  for (const unsigned char ch : value) {
    if (std::isalnum(ch) != 0) {
      normalized.push_back(static_cast<char>(ch));
      last_was_space = false;
      continue;
    }

    if (!last_was_space) {
      normalized.push_back(' ');
      last_was_space = true;
    }
  }

  while (!normalized.empty() && normalized.front() == ' ') {
    normalized.erase(normalized.begin());
  }
  while (!normalized.empty() && normalized.back() == ' ') {
    normalized.pop_back();
  }

  return normalized;
}

auto extension_is(const std::filesystem::path& path, std::string_view extension) -> bool {
  return lower_ascii(path.extension().string()) == extension;
}

}  // namespace

auto resolve_convolver_kernel(const ConvolverPreset& preset, std::string& warning) -> std::optional<ResolvedKernel> {
  const std::string kernel_name = derive_kernel_name(preset);
  if (kernel_name.empty()) {
    warning = "convolver has no kernel-name or kernel-path; skipping convolver";
    return std::nullopt;
  }

  const auto base_dir = app_data_dir() / "irs";
  const auto irs_path = base_dir / (kernel_name + ".irs");
  const auto sofa_path = base_dir / (kernel_name + ".sofa");

  if (std::filesystem::exists(irs_path)) {
    return ResolvedKernel{.name = kernel_name, .path = irs_path.string(), .is_sofa = false};
  }

  if (std::filesystem::exists(sofa_path)) {
    warning = "SOFA kernel found but SOFA convolver support is not implemented yet: " + kernel_name;
    return std::nullopt;
  }

  if (std::filesystem::exists(base_dir) && std::filesystem::is_directory(base_dir)) {
    const auto wanted = normalize_name(kernel_name);

    for (const auto& entry : std::filesystem::directory_iterator(base_dir)) {
      if (!entry.is_regular_file()) {
        continue;
      }

      const auto path = entry.path();
      if (!extension_is(path, ".irs") && !extension_is(path, ".sofa")) {
        continue;
      }

      if (normalize_name(path.stem().string()) != wanted) {
        continue;
      }

      if (extension_is(path, ".sofa")) {
        warning = "SOFA kernel matched by fuzzy name but SOFA support is not implemented yet: " + path.filename().string();
        return std::nullopt;
      }

      warning = "convolver kernel exact name not found; using fuzzy local match: " + path.filename().string();
      return ResolvedKernel{.name = path.stem().string(), .path = path.string(), .is_sofa = false};
    }
  }

  warning = "convolver kernel not found under " + base_dir.string() + ": " + kernel_name;
  return std::nullopt;
}

}  // namespace ee
