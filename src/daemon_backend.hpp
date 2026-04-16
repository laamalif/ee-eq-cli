#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ee_eq_preset_parser.hpp"

namespace ee {

struct RuntimeSnapshot {
  bool session_active = false;
  std::string preset_origin;
  std::string sink_name;
  uint64_t sink_serial = 0;
  std::vector<std::string> active_plugins;
  bool bypass = false;
};

class SessionBackend {
 public:
  virtual ~SessionBackend() = default;

  virtual auto start_session(const ParsedPreset& preset,
                             std::string preset_origin,
                             std::string sink_selector,
                             std::string& error) -> bool = 0;
  virtual void stop_session() = 0;
  virtual void set_bypass(bool bypass) = 0;
  virtual auto list_sinks(std::string& error) -> std::vector<std::string> = 0;
  [[nodiscard]] virtual auto snapshot() const -> RuntimeSnapshot = 0;
};

auto make_real_session_backend() -> std::unique_ptr<SessionBackend>;

}  // namespace ee
