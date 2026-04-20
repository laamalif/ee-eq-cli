#pragma once

#include <cstdlib>
#include <cstring>
#include <string_view>

namespace ee {

constexpr auto kApplicationName = "eq-cli";
constexpr auto kApplicationVersion = "0.3.1.8";
constexpr auto kDefaultPresetEnv = "EQ_CLI_DEFAULT_PRESET";
constexpr auto kLegacyDefaultPresetEnv = "EE_EQ_CLI_DEFAULT_PRESET";
constexpr auto kDisableConvolverEnv = "EQ_CLI_DISABLE_CONVOLVER";
constexpr auto kLegacyDisableConvolverEnv = "EE_EQ_CLI_DISABLE_CONVOLVER";
constexpr auto kConvolverRtProcessEnv = "EQ_CLI_CONVOLVER_RT_PROCESS";
constexpr auto kLegacyConvolverRtProcessEnv = "EE_EQ_CLI_CONVOLVER_RT_PROCESS";
constexpr auto kConvolverSchedFifoEnv = "EQ_CLI_CONVOLVER_SCHED_FIFO";
constexpr auto kLegacyConvolverSchedFifoEnv = "EE_EQ_CLI_CONVOLVER_SCHED_FIFO";
constexpr auto kRuntimeDirName = "eq-cli";
constexpr auto kLegacyRuntimeDirName = "ee-eq-cli";

struct CompatEnvValue {
  const char* value = nullptr;
  std::string_view name{};
  bool used_legacy = false;

  explicit operator bool() const { return value != nullptr; }
};

inline auto read_compat_env(const char* primary_name, const char* legacy_name) -> CompatEnvValue {
  if (const char* value = std::getenv(primary_name); value != nullptr && *value != '\0') {
    return {.value = value, .name = primary_name, .used_legacy = false};
  }
  if (const char* value = std::getenv(legacy_name); value != nullptr && *value != '\0') {
    return {.value = value, .name = legacy_name, .used_legacy = true};
  }
  return {};
}

inline auto compat_env_enabled(const CompatEnvValue& env) -> bool {
  return env.value != nullptr && *env.value != '\0' && std::strcmp(env.value, "0") != 0;
}

}  // namespace ee
