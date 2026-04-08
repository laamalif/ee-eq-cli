#pragma once

#include <lilv/lilv.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/lv2plug.in/ns/ext/log/log.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/urid/urid.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ee {

class Lv2HostCore {
 public:
  enum class PortType { Control, Audio, Atom };

  struct Port {
    PortType type{};
    uint32_t index = 0;
    std::string name;
    std::string symbol;
    float value = 0.0F;
    float min = -std::numeric_limits<float>::infinity();
    float max = std::numeric_limits<float>::infinity();
    bool is_input = false;
    bool optional = false;
  };

  explicit Lv2HostCore(std::string plugin_uri);
  ~Lv2HostCore();

  Lv2HostCore(const Lv2HostCore&) = delete;
  auto operator=(const Lv2HostCore&) -> Lv2HostCore& = delete;

  auto found_plugin() const -> bool { return found_plugin_; }
  auto init_error() const -> const std::string& { return init_error_; }
  auto create_instance(uint32_t sample_rate, uint32_t block_size) -> bool;
  void destroy_instance();

  auto has_instance() const -> bool { return instance_ != nullptr; }
  auto sample_rate() const -> uint32_t { return sample_rate_; }
  auto block_size() const -> uint32_t { return block_size_; }

  void set_control_port_value(const std::string& symbol, float value);
  auto get_control_port_value(const std::string& symbol) -> float;
  void connect_audio_ports(std::span<float> left_in,
                           std::span<float> right_in,
                           std::span<float> left_out,
                           std::span<float> right_out);
  void connect_audio_ports(std::span<float> left_in,
                           std::span<float> right_in,
                           std::span<float> left_out,
                           std::span<float> right_out,
                           std::span<float> probe_left,
                           std::span<float> probe_right);
  void run() const;

 private:
  static constexpr int32_t kMinQuantum = 32;
  static constexpr int32_t kMaxQuantum = 8192;

  std::string plugin_uri_;
  LilvWorld* world_ = nullptr;
  const LilvPlugin* plugin_ = nullptr;
  LilvInstance* instance_ = nullptr;
  bool found_plugin_ = false;
  std::string init_error_;
  uint32_t sample_rate_ = 0;
  uint32_t block_size_ = 0;
  float sample_rate_option_ = 0.0F;
  LV2_URID next_urid_ = 1;

  std::unordered_map<std::string, LV2_URID> uri_to_urid_;
  std::unordered_map<LV2_URID, std::string> urid_to_uri_;
  std::vector<Port> ports_;

  struct {
    struct {
      uint32_t left = UINT32_MAX;
      uint32_t right = UINT32_MAX;
    } in;
    struct {
      uint32_t left = UINT32_MAX;
      uint32_t right = UINT32_MAX;
    } probe;
    struct {
      uint32_t left = UINT32_MAX;
      uint32_t right = UINT32_MAX;
    } out;
  } audio_ports_{};

  void check_required_features();
  void create_ports();
  void connect_control_ports();
  auto map_urid(const std::string& uri) -> LV2_URID;
};

}  // namespace ee
