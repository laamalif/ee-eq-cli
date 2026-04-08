#include "lv2_host_core.hpp"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/options/options.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <format>

#include "logging.hpp"

namespace ee {

namespace {

auto lv2_printf([[maybe_unused]] LV2_Log_Handle handle, [[maybe_unused]] LV2_URID type, const char* format, ...)
    -> int {
  va_list args;
  va_start(args, format);
  const int result = std::vprintf(format, args);
  va_end(args);
  return result;
}

}  // namespace

Lv2HostCore::Lv2HostCore(std::string plugin_uri) : plugin_uri_(std::move(plugin_uri)) {
  world_ = lilv_world_new();
  if (world_ == nullptr) {
    log::error("failed to initialize Lilv world");
    return;
  }

  auto* uri = lilv_new_uri(world_, plugin_uri_.c_str());
  if (uri == nullptr) {
    log::error(std::format("invalid LV2 plugin URI: {}", plugin_uri_));
    return;
  }

  lilv_world_load_all(world_);
  const LilvPlugins* plugins = lilv_world_get_all_plugins(world_);
  plugin_ = lilv_plugins_get_by_uri(plugins, uri);
  lilv_node_free(uri);

  if (plugin_ == nullptr) {
    log::error(std::format("could not find LV2 plugin: {}", plugin_uri_));
    return;
  }

  found_plugin_ = true;
  check_required_features();
  create_ports();
}

Lv2HostCore::~Lv2HostCore() {
  destroy_instance();
  if (world_ != nullptr) {
    lilv_world_free(world_);
  }
}

void Lv2HostCore::check_required_features() {
  LilvNodes* required_features = lilv_plugin_get_required_features(plugin_);
  if (required_features == nullptr) {
    return;
  }

  lilv_nodes_free(required_features);
}

void Lv2HostCore::create_ports() {
  const auto n_ports = lilv_plugin_get_num_ports(plugin_);
  ports_.resize(n_ports);

  std::vector<float> values(n_ports);
  std::vector<float> minimum(n_ports);
  std::vector<float> maximum(n_ports);
  lilv_plugin_get_port_ranges_float(plugin_, minimum.data(), maximum.data(), values.data());

  LilvNode* input_port = lilv_new_uri(world_, LV2_CORE__InputPort);
  LilvNode* output_port = lilv_new_uri(world_, LV2_CORE__OutputPort);
  LilvNode* audio_port = lilv_new_uri(world_, LV2_CORE__AudioPort);
  LilvNode* control_port = lilv_new_uri(world_, LV2_CORE__ControlPort);
  LilvNode* atom_port = lilv_new_uri(world_, LV2_ATOM__AtomPort);
  LilvNode* optional_port = lilv_new_uri(world_, LV2_CORE__connectionOptional);

  uint32_t n_audio_in = 0;
  uint32_t n_audio_out = 0;

  for (uint32_t i = 0; i < n_ports; ++i) {
    auto* port = &ports_[i];
    const auto* lilv_port = lilv_plugin_get_port_by_index(plugin_, i);
    auto* port_name = lilv_port_get_name(plugin_, lilv_port);

    port->index = i;
    port->name = lilv_node_as_string(port_name);
    port->symbol = lilv_node_as_string(lilv_port_get_symbol(plugin_, lilv_port));
    port->optional = lilv_port_has_property(plugin_, lilv_port, optional_port);

    if (!std::isnan(values[i])) {
      port->value = values[i];
    }
    if (!std::isnan(minimum[i])) {
      port->min = minimum[i];
    }
    if (!std::isnan(maximum[i])) {
      port->max = maximum[i];
    }

    if (lilv_port_is_a(plugin_, lilv_port, input_port)) {
      port->is_input = true;
    }

    if (lilv_port_is_a(plugin_, lilv_port, control_port)) {
      port->type = PortType::Control;
    } else if (lilv_port_is_a(plugin_, lilv_port, atom_port)) {
      port->type = PortType::Atom;
    } else if (lilv_port_is_a(plugin_, lilv_port, audio_port)) {
      port->type = PortType::Audio;
      if (port->is_input) {
        if (n_audio_in == 0) {
          audio_ports_.in.left = i;
        } else if (n_audio_in == 1) {
          audio_ports_.in.right = i;
        } else if (n_audio_in == 2) {
          audio_ports_.probe.left = i;
        } else if (n_audio_in == 3) {
          audio_ports_.probe.right = i;
        }
        ++n_audio_in;
      } else {
        if (n_audio_out == 0) {
          audio_ports_.out.left = i;
        } else if (n_audio_out == 1) {
          audio_ports_.out.right = i;
        }
        ++n_audio_out;
      }
    }

    lilv_node_free(port_name);
  }

  lilv_node_free(optional_port);
  lilv_node_free(control_port);
  lilv_node_free(atom_port);
  lilv_node_free(audio_port);
  lilv_node_free(output_port);
  lilv_node_free(input_port);
}

auto Lv2HostCore::map_urid(const std::string& uri) -> LV2_URID {
  const auto it = uri_to_urid_.find(uri);
  if (it != uri_to_urid_.end()) {
    return it->second;
  }

  const auto urid = next_urid_++;
  uri_to_urid_[uri] = urid;
  urid_to_uri_[urid] = uri;
  return urid;
}

auto Lv2HostCore::create_instance(uint32_t sample_rate, uint32_t block_size) -> bool {
  if (!found_plugin_) {
    return false;
  }
  if (instance_ != nullptr && sample_rate_ == sample_rate && block_size_ == block_size) {
    return true;
  }

  destroy_instance();
  sample_rate_ = sample_rate;
  block_size_ = block_size;
  sample_rate_option_ = static_cast<float>(sample_rate_);

  LV2_Log_Log lv2_log = {.handle = this,
                         .printf = &lv2_printf,
                         .vprintf = []([[maybe_unused]] LV2_Log_Handle handle, [[maybe_unused]] LV2_URID type,
                                       const char* fmt, va_list ap) { return std::vprintf(fmt, ap); }};

  LV2_URID_Map lv2_map = {.handle = this,
                          .map = [](LV2_URID_Map_Handle handle, const char* uri) {
                            return static_cast<Lv2HostCore*>(handle)->map_urid(uri);
                          }};

  LV2_URID_Unmap lv2_unmap = {.handle = this,
                              .unmap = [](LV2_URID_Unmap_Handle handle, LV2_URID urid) {
                                auto* self = static_cast<Lv2HostCore*>(handle);
                                const auto it = self->urid_to_uri_.find(urid);
                                return it != self->urid_to_uri_.end() ? it->second.c_str() : nullptr;
                              }};

  const LV2_Feature log_feature = {.URI = LV2_LOG__log, .data = &lv2_log};
  const LV2_Feature map_feature = {.URI = LV2_URID__map, .data = &lv2_map};
  const LV2_Feature unmap_feature = {.URI = LV2_URID__unmap, .data = &lv2_unmap};

  auto options = std::to_array<LV2_Options_Option>(
      {{.context = LV2_OPTIONS_INSTANCE,
        .subject = 0,
        .key = map_urid(LV2_PARAMETERS__sampleRate),
        .size = sizeof(float),
        .type = map_urid(LV2_ATOM__Float),
        .value = &sample_rate_option_},
       {.context = LV2_OPTIONS_INSTANCE,
        .subject = 0,
        .key = map_urid(LV2_BUF_SIZE__minBlockLength),
        .size = sizeof(int32_t),
        .type = map_urid(LV2_ATOM__Int),
        .value = &kMinQuantum},
       {.context = LV2_OPTIONS_INSTANCE,
        .subject = 0,
        .key = map_urid(LV2_BUF_SIZE__maxBlockLength),
        .size = sizeof(int32_t),
        .type = map_urid(LV2_ATOM__Int),
        .value = &kMaxQuantum},
       {.context = LV2_OPTIONS_INSTANCE,
        .subject = 0,
        .key = map_urid(LV2_BUF_SIZE__nominalBlockLength),
        .size = sizeof(int32_t),
        .type = map_urid(LV2_ATOM__Int),
        .value = &block_size_},
       {.context = LV2_OPTIONS_INSTANCE, .subject = 0, .key = 0, .size = 0, .type = 0, .value = nullptr}});

  const LV2_Feature options_feature = {.URI = LV2_OPTIONS__options, .data = options.data()};
  const LV2_Feature bounded_block_feature = {.URI = LV2_BUF_SIZE__boundedBlockLength, .data = nullptr};

  const auto features = std::to_array<const LV2_Feature*>(
      {&log_feature, &map_feature, &unmap_feature, &options_feature, &bounded_block_feature, nullptr});

  instance_ = lilv_plugin_instantiate(plugin_, sample_rate_, features.data());
  if (instance_ == nullptr) {
    log::error(std::format("failed to instantiate LV2 plugin: {}", plugin_uri_));
    return false;
  }

  connect_control_ports();
  lilv_instance_activate(instance_);
  return true;
}

void Lv2HostCore::destroy_instance() {
  if (instance_ != nullptr) {
    lilv_instance_deactivate(instance_);
    lilv_instance_free(instance_);
    instance_ = nullptr;
  }
}

void Lv2HostCore::connect_control_ports() {
  for (auto& port : ports_) {
    if (port.type == PortType::Control) {
      lilv_instance_connect_port(instance_, port.index, &port.value);
    }
  }
}

void Lv2HostCore::connect_audio_ports(std::span<float> left_in,
                                      std::span<float> right_in,
                                      std::span<float> left_out,
                                      std::span<float> right_out) {
  if (instance_ == nullptr) {
    return;
  }

  if (audio_ports_.in.left != UINT32_MAX) {
    lilv_instance_connect_port(instance_, audio_ports_.in.left, left_in.data());
  }
  if (audio_ports_.in.right != UINT32_MAX) {
    lilv_instance_connect_port(instance_, audio_ports_.in.right, right_in.data());
  }
  if (audio_ports_.out.left != UINT32_MAX) {
    lilv_instance_connect_port(instance_, audio_ports_.out.left, left_out.data());
  }
  if (audio_ports_.out.right != UINT32_MAX) {
    lilv_instance_connect_port(instance_, audio_ports_.out.right, right_out.data());
  }
}

void Lv2HostCore::connect_audio_ports(std::span<float> left_in,
                                      std::span<float> right_in,
                                      std::span<float> left_out,
                                      std::span<float> right_out,
                                      std::span<float> probe_left,
                                      std::span<float> probe_right) {
  connect_audio_ports(left_in, right_in, left_out, right_out);
  if (instance_ == nullptr) {
    return;
  }

  if (audio_ports_.probe.left != UINT32_MAX) {
    lilv_instance_connect_port(instance_, audio_ports_.probe.left, probe_left.data());
  }
  if (audio_ports_.probe.right != UINT32_MAX) {
    lilv_instance_connect_port(instance_, audio_ports_.probe.right, probe_right.data());
  }
}

void Lv2HostCore::run() const {
  if (instance_ != nullptr) {
    lilv_instance_run(instance_, block_size_);
  }
}

void Lv2HostCore::set_control_port_value(const std::string& symbol, float value) {
  for (auto& port : ports_) {
    if (port.type == PortType::Control && port.symbol == symbol) {
      if (!port.is_input) {
        return;
      }
      port.value = std::clamp(value, port.min, port.max);
      return;
    }
  }
}

auto Lv2HostCore::get_control_port_value(const std::string& symbol) -> float {
  for (const auto& port : ports_) {
    if (port.type == PortType::Control && port.symbol == symbol) {
      return port.value;
    }
  }
  return 0.0F;
}

}  // namespace ee
