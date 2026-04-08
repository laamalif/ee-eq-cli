#include "pipewire_router.hpp"

#include <pipewire/context.h>
#include <pipewire/core.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/filter.h>
#include <pipewire/keys.h>
#include <pipewire/link.h>
#include <pipewire/node.h>
#include <pipewire/pipewire.h>
#include <pipewire/port.h>
#include <pipewire/properties.h>
#include <pipewire/proxy.h>
#include <pipewire/thread-loop.h>
#include <spa/param/format.h>
#include <spa/pod/builder.h>
#include <spa/utils/dict.h>
#include <spa/utils/hook.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sys/eventfd.h>
#include <unistd.h>
#include <format>
#include <memory>
#include <nlohmann/json.hpp>
#include <ranges>
#include <span>
#include <thread>
#include <vector>

#include "logging.hpp"
#include "convolver_host.hpp"
#include "kernel_resolver.hpp"
#include "lsp_eq_port_mapper.hpp"
#include "lsp_limiter_port_mapper.hpp"
#include "lv2_host_core.hpp"
#include "math_utils.hpp"

namespace ee {

namespace {

constexpr auto kAppId = "com.github.wwmm.ee-eq-cli";
constexpr auto kVirtualSinkName = "ee_eq_cli_sink";
constexpr auto kVirtualSinkDescription = "ee-eq-cli Sink";
constexpr auto kEqNodeName = "ee_eq_cli_equalizer";
constexpr auto kEqPluginUri = "http://lsp-plug.in/plugins/lv2/para_equalizer_x32_lr";
constexpr auto kLimiterNodeName = "ee_eq_cli_limiter";
constexpr auto kLimiterPluginUri = "http://lsp-plug.in/plugins/lv2/sc_limiter_stereo";
constexpr auto kRapidRemovalWindow = std::chrono::milliseconds(1500);
constexpr auto kLoopSuppressDuration = std::chrono::seconds(5);
constexpr auto kLoopSuppressThreshold = 3;

struct FilterPort {
  void* data = nullptr;
};

auto make_port_props(const char* name, const char* channel) -> pw_properties* {
  auto* props = pw_properties_new(nullptr, nullptr);
  pw_properties_set(props, PW_KEY_FORMAT_DSP, "32 bit float mono audio");
  pw_properties_set(props, PW_KEY_PORT_NAME, name);
  pw_properties_set(props, "audio.channel", channel);
  return props;
}

auto parse_uint32(const char* value) -> uint32_t {
  if (value == nullptr) {
    return 0;
  }
  return static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
}

auto parse_uint64(const char* value) -> uint64_t {
  if (value == nullptr) {
    return 0;
  }
  return static_cast<uint64_t>(std::strtoull(value, nullptr, 10));
}

auto convolver_disabled_by_env() -> bool {
  if (const char* value = std::getenv("EE_EQ_CLI_DISABLE_CONVOLVER"); value != nullptr) {
    return *value != '\0' && std::strcmp(value, "0") != 0;
  }
  return false;
}

auto use_rt_convolver_filter() -> bool {
  if (const char* value = std::getenv("EE_EQ_CLI_CONVOLVER_RT_PROCESS"); value != nullptr) {
    return *value != '\0' && std::strcmp(value, "0") != 0;
  }
  return false;
}

}  // namespace

struct PipeWireRouter::NodeData {
  PipeWireRouter* router = nullptr;
  pw_proxy* proxy = nullptr;
  spa_hook object_listener{};
  spa_hook proxy_listener{};
  NodeInfo* info = nullptr;
  std::string last_logged_target;
  bool observation_logged = false;
  bool removed = false;
};

class PipeWireRouter::EqFilterNode {
 public:
  EqFilterNode(pw_core* core, pw_thread_loop* thread_loop, const EqPreset& preset)
      : core_(core), thread_loop_(thread_loop), preset_(preset), host_(kEqPluginUri) {}

  ~EqFilterNode() {
    disconnect();
  }

  auto connect(std::string& error) -> bool {
    if (!host_.found_plugin()) {
      error = host_.init_error().empty() ? "LSP LV2 equalizer plugin is not installed or discoverable" : host_.init_error();
      return false;
    }

    pw_thread_loop_lock(thread_loop_);

    auto* props = pw_properties_new(nullptr, nullptr);
    pw_properties_set(props, PW_KEY_APP_ID, kAppId);
    pw_properties_set(props, PW_KEY_NODE_NAME, kEqNodeName);
    pw_properties_set(props, PW_KEY_NODE_NICK, "equalizer");
    pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, "ee-eq-cli Equalizer");
    pw_properties_set(props, PW_KEY_MEDIA_TYPE, "Audio");
    pw_properties_set(props, PW_KEY_MEDIA_CATEGORY, "Duplex");
    pw_properties_set(props, PW_KEY_MEDIA_ROLE, "DSP");
    pw_properties_set(props, PW_KEY_NODE_GROUP, "ee_eq_cli_group");
    pw_properties_set(props, PW_KEY_NODE_PASSIVE, "true");

    filter_ = pw_filter_new(core_, kEqNodeName, props);
    if (filter_ == nullptr) {
      pw_thread_loop_unlock(thread_loop_);
      error = "failed to create PipeWire EQ filter node";
      return false;
    }

    left_in_ = static_cast<FilterPort*>(pw_filter_add_port(filter_, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                                           sizeof(FilterPort), make_port_props("input_FL", "FL"), nullptr,
                                                           0));
    right_in_ = static_cast<FilterPort*>(pw_filter_add_port(filter_, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                                            sizeof(FilterPort), make_port_props("input_FR", "FR"), nullptr,
                                                            0));
    left_out_ = static_cast<FilterPort*>(pw_filter_add_port(filter_, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                                            sizeof(FilterPort), make_port_props("output_FL", "FL"),
                                                            nullptr, 0));
    right_out_ = static_cast<FilterPort*>(pw_filter_add_port(
        filter_, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(FilterPort), make_port_props("output_FR", "FR"),
        nullptr, 0));

    pw_filter_add_listener(filter_, &listener_, &filter_events_, this);

    if (pw_filter_connect(filter_, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) != 0) {
      pw_filter_destroy(filter_);
      filter_ = nullptr;
      pw_thread_loop_unlock(thread_loop_);
      error = "failed to connect PipeWire EQ filter";
      return false;
    }

    pw_core_sync(core_, PW_ID_CORE, 0);
    pw_thread_loop_wait(thread_loop_);
    pw_thread_loop_unlock(thread_loop_);

    for (int i = 0; i < 5000 && !can_get_node_id_; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      if (state_ == PW_FILTER_STATE_ERROR) {
        error = "EQ filter entered PipeWire error state";
        return false;
      }
    }
    if (!can_get_node_id_) {
      error = "timed out waiting for EQ filter node to become usable";
      return false;
    }

    pw_thread_loop_lock(thread_loop_);
    node_id_ = pw_filter_get_node_id(filter_);
    pw_core_sync(core_, PW_ID_CORE, 0);
    pw_thread_loop_wait(thread_loop_);
    pw_thread_loop_unlock(thread_loop_);

    return true;
  }

  void disconnect() {
    if (filter_ == nullptr) {
      return;
    }

    pw_thread_loop_lock(thread_loop_);
    pw_filter_disconnect(filter_);
    pw_filter_destroy(filter_);
    pw_thread_loop_unlock(thread_loop_);
    filter_ = nullptr;
    node_id_ = SPA_ID_INVALID;
  }

  auto node_id() const -> uint32_t {
    return node_id_;
  }

 private:
  static void on_filter_state_changed(void* userdata,
                                      [[maybe_unused]] pw_filter_state old_state,
                                      pw_filter_state state,
                                      [[maybe_unused]] const char* error) {
    auto* self = static_cast<EqFilterNode*>(userdata);
    self->state_ = state;
    self->can_get_node_id_ = (state == PW_FILTER_STATE_STREAMING || state == PW_FILTER_STATE_PAUSED);
  }

  static void on_process(void* userdata, spa_io_position* position) {
    auto* self = static_cast<EqFilterNode*>(userdata);
    const auto n_samples = position->clock.duration;
    const auto rate = position->clock.rate.denom;
    if (n_samples == 0 || rate == 0) {
      return;
    }

    self->ensure_instance(rate, n_samples);

    auto* in_left = static_cast<float*>(pw_filter_get_dsp_buffer(self->left_in_, n_samples));
    auto* in_right = static_cast<float*>(pw_filter_get_dsp_buffer(self->right_in_, n_samples));
    auto* out_left = static_cast<float*>(pw_filter_get_dsp_buffer(self->left_out_, n_samples));
    auto* out_right = static_cast<float*>(pw_filter_get_dsp_buffer(self->right_out_, n_samples));

    self->scratch_left_.resize(n_samples);
    self->scratch_right_.resize(n_samples);
    self->dummy_left_.assign(n_samples, 0.0F);
    self->dummy_right_.assign(n_samples, 0.0F);

    std::span<float> left_in = in_left != nullptr ? std::span<float>(in_left, n_samples) : std::span<float>(self->dummy_left_);
    std::span<float> right_in =
        in_right != nullptr ? std::span<float>(in_right, n_samples) : std::span<float>(self->dummy_right_);
    std::span<float> left_out =
        out_left != nullptr ? std::span<float>(out_left, n_samples) : std::span<float>(self->dummy_left_);
    std::span<float> right_out =
        out_right != nullptr ? std::span<float>(out_right, n_samples) : std::span<float>(self->dummy_right_);

    if (self->preset_.bypass) {
      std::ranges::copy(left_in, left_out.begin());
      std::ranges::copy(right_in, right_out.begin());
      return;
    }

    std::ranges::copy(left_in, self->scratch_left_.begin());
    std::ranges::copy(right_in, self->scratch_right_.begin());

    if (!self->ready_) {
      std::ranges::copy(self->scratch_left_, left_out.begin());
      std::ranges::copy(self->scratch_right_, right_out.begin());
      if (self->output_gain_ != 1.0F) {
        self->apply_gain(left_out, right_out, self->output_gain_);
      }
      return;
    }

    if (self->input_gain_ != 1.0F) {
      self->apply_gain(self->scratch_left_, self->scratch_right_, self->input_gain_);
    }

    self->host_.connect_audio_ports(self->scratch_left_, self->scratch_right_, left_out, right_out);
    self->host_.run();

    if (self->output_gain_ != 1.0F) {
      self->apply_gain(left_out, right_out, self->output_gain_);
    }
  }

  void ensure_instance(uint32_t rate, uint32_t n_samples) {
    if (ready_ && rate == rate_ && n_samples == n_samples_) {
      return;
    }

    rate_ = rate;
    n_samples_ = n_samples;
    ready_ = host_.create_instance(rate_, n_samples_);
    if (ready_) {
      apply_eq_preset_to_host(preset_, host_);
    }
  }

  void apply_gain(std::span<float> left, std::span<float> right, const float gain) const {
    for (size_t i = 0; i < left.size(); ++i) {
      left[i] *= gain;
      right[i] *= gain;
    }
  }

  pw_core* core_ = nullptr;
  pw_thread_loop* thread_loop_ = nullptr;
  EqPreset preset_;
  Lv2HostCore host_;
  pw_filter* filter_ = nullptr;
  spa_hook listener_{};
  FilterPort* left_in_ = nullptr;
  FilterPort* right_in_ = nullptr;
  FilterPort* left_out_ = nullptr;
  FilterPort* right_out_ = nullptr;
  uint32_t node_id_ = SPA_ID_INVALID;
  uint32_t rate_ = 0;
  uint32_t n_samples_ = 0;
  bool ready_ = false;
  bool can_get_node_id_ = false;
  pw_filter_state state_ = PW_FILTER_STATE_UNCONNECTED;
  float input_gain_ = static_cast<float>(ee::math::db_to_linear(preset_.input_gain_db));
  float output_gain_ = static_cast<float>(ee::math::db_to_linear(preset_.output_gain_db));
  std::vector<float> scratch_left_;
  std::vector<float> scratch_right_;
  std::vector<float> dummy_left_;
  std::vector<float> dummy_right_;

  const pw_filter_events filter_events_ = {
      .version = 0,
      .destroy = nullptr,
      .state_changed = &EqFilterNode::on_filter_state_changed,
      .io_changed = nullptr,
      .param_changed = nullptr,
      .add_buffer = nullptr,
      .remove_buffer = nullptr,
      .process = &EqFilterNode::on_process,
      .drained = nullptr,
      .command = nullptr,
  };
};

class PipeWireRouter::LimiterFilterNode {
 public:
  LimiterFilterNode(pw_core* core, pw_thread_loop* thread_loop, const LimiterPreset& preset)
      : core_(core), thread_loop_(thread_loop), preset_(preset), host_(kLimiterPluginUri) {}

  ~LimiterFilterNode() {
    disconnect();
  }

  auto connect(std::string& error) -> bool {
    if (!host_.found_plugin()) {
      error = host_.init_error().empty() ? "LSP LV2 limiter plugin is not installed or discoverable" : host_.init_error();
      return false;
    }

    pw_thread_loop_lock(thread_loop_);

    auto* props = pw_properties_new(nullptr, nullptr);
    pw_properties_set(props, PW_KEY_APP_ID, kAppId);
    pw_properties_set(props, PW_KEY_NODE_NAME, kLimiterNodeName);
    pw_properties_set(props, PW_KEY_NODE_NICK, "limiter");
    pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, "ee-eq-cli Limiter");
    pw_properties_set(props, PW_KEY_MEDIA_TYPE, "Audio");
    pw_properties_set(props, PW_KEY_MEDIA_CATEGORY, "Duplex");
    pw_properties_set(props, PW_KEY_MEDIA_ROLE, "DSP");
    pw_properties_set(props, PW_KEY_NODE_GROUP, "ee_eq_cli_group");
    pw_properties_set(props, PW_KEY_NODE_PASSIVE, "true");

    filter_ = pw_filter_new(core_, kLimiterNodeName, props);
    if (filter_ == nullptr) {
      pw_thread_loop_unlock(thread_loop_);
      error = "failed to create PipeWire limiter filter node";
      return false;
    }

    left_in_ = static_cast<FilterPort*>(pw_filter_add_port(filter_, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                                           sizeof(FilterPort), make_port_props("input_FL", "FL"),
                                                           nullptr, 0));
    right_in_ = static_cast<FilterPort*>(pw_filter_add_port(filter_, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                                            sizeof(FilterPort), make_port_props("input_FR", "FR"),
                                                            nullptr, 0));
    probe_left_ = static_cast<FilterPort*>(pw_filter_add_port(filter_, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                                              sizeof(FilterPort),
                                                              make_port_props("probe_FL", "PROBE_FL"), nullptr,
                                                              0));
    probe_right_ = static_cast<FilterPort*>(pw_filter_add_port(
        filter_, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(FilterPort),
        make_port_props("probe_FR", "PROBE_FR"), nullptr, 0));
    left_out_ = static_cast<FilterPort*>(pw_filter_add_port(filter_, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                                            sizeof(FilterPort), make_port_props("output_FL", "FL"),
                                                            nullptr, 0));
    right_out_ = static_cast<FilterPort*>(pw_filter_add_port(
        filter_, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(FilterPort),
        make_port_props("output_FR", "FR"), nullptr, 0));

    pw_filter_add_listener(filter_, &listener_, &filter_events_, this);

    if (pw_filter_connect(filter_, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) != 0) {
      pw_filter_destroy(filter_);
      filter_ = nullptr;
      pw_thread_loop_unlock(thread_loop_);
      error = "failed to connect PipeWire limiter filter";
      return false;
    }

    pw_core_sync(core_, PW_ID_CORE, 0);
    pw_thread_loop_wait(thread_loop_);
    pw_thread_loop_unlock(thread_loop_);

    for (int i = 0; i < 5000 && !can_get_node_id_; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      if (state_ == PW_FILTER_STATE_ERROR) {
        error = "Limiter filter entered PipeWire error state";
        return false;
      }
    }
    if (!can_get_node_id_) {
      error = "timed out waiting for limiter filter node to become usable";
      return false;
    }

    pw_thread_loop_lock(thread_loop_);
    node_id_ = pw_filter_get_node_id(filter_);
    pw_core_sync(core_, PW_ID_CORE, 0);
    pw_thread_loop_wait(thread_loop_);
    pw_thread_loop_unlock(thread_loop_);

    return true;
  }

  void disconnect() {
    if (filter_ == nullptr) {
      return;
    }

    pw_thread_loop_lock(thread_loop_);
    pw_filter_disconnect(filter_);
    pw_filter_destroy(filter_);
    pw_thread_loop_unlock(thread_loop_);
    filter_ = nullptr;
    node_id_ = SPA_ID_INVALID;
  }

  auto node_id() const -> uint32_t {
    return node_id_;
  }

 private:
  static void on_filter_state_changed(void* userdata,
                                      [[maybe_unused]] pw_filter_state old_state,
                                      pw_filter_state state,
                                      [[maybe_unused]] const char* error) {
    auto* self = static_cast<LimiterFilterNode*>(userdata);
    self->state_ = state;
    self->can_get_node_id_ = (state == PW_FILTER_STATE_STREAMING || state == PW_FILTER_STATE_PAUSED);
  }

  static void on_process(void* userdata, spa_io_position* position) {
    auto* self = static_cast<LimiterFilterNode*>(userdata);
    const auto n_samples = position->clock.duration;
    const auto rate = position->clock.rate.denom;
    if (n_samples == 0 || rate == 0) {
      return;
    }

    self->ensure_instance(rate, n_samples);

    auto* in_left = static_cast<float*>(pw_filter_get_dsp_buffer(self->left_in_, n_samples));
    auto* in_right = static_cast<float*>(pw_filter_get_dsp_buffer(self->right_in_, n_samples));
    auto* probe_left = static_cast<float*>(pw_filter_get_dsp_buffer(self->probe_left_, n_samples));
    auto* probe_right = static_cast<float*>(pw_filter_get_dsp_buffer(self->probe_right_, n_samples));
    auto* out_left = static_cast<float*>(pw_filter_get_dsp_buffer(self->left_out_, n_samples));
    auto* out_right = static_cast<float*>(pw_filter_get_dsp_buffer(self->right_out_, n_samples));

    self->scratch_left_.resize(n_samples);
    self->scratch_right_.resize(n_samples);
    self->dummy_left_.assign(n_samples, 0.0F);
    self->dummy_right_.assign(n_samples, 0.0F);

    std::span<float> left_in = in_left != nullptr ? std::span<float>(in_left, n_samples) : std::span<float>(self->dummy_left_);
    std::span<float> right_in =
        in_right != nullptr ? std::span<float>(in_right, n_samples) : std::span<float>(self->dummy_right_);
    std::span<float> side_left =
        probe_left != nullptr ? std::span<float>(probe_left, n_samples) : std::span<float>(self->dummy_left_);
    std::span<float> side_right =
        probe_right != nullptr ? std::span<float>(probe_right, n_samples) : std::span<float>(self->dummy_right_);
    std::span<float> left_out =
        out_left != nullptr ? std::span<float>(out_left, n_samples) : std::span<float>(self->dummy_left_);
    std::span<float> right_out =
        out_right != nullptr ? std::span<float>(out_right, n_samples) : std::span<float>(self->dummy_right_);

    if (self->preset_.bypass) {
      std::ranges::copy(left_in, left_out.begin());
      std::ranges::copy(right_in, right_out.begin());
      return;
    }

    std::ranges::copy(left_in, self->scratch_left_.begin());
    std::ranges::copy(right_in, self->scratch_right_.begin());

    if (!self->ready_) {
      std::ranges::copy(self->scratch_left_, left_out.begin());
      std::ranges::copy(self->scratch_right_, right_out.begin());
      if (self->output_gain_ != 1.0F) {
        self->apply_gain(left_out, right_out, self->output_gain_);
      }
      return;
    }

    if (self->input_gain_ != 1.0F) {
      self->apply_gain(self->scratch_left_, self->scratch_right_, self->input_gain_);
    }

    self->host_.connect_audio_ports(self->scratch_left_, self->scratch_right_, left_out, right_out, side_left, side_right);
    self->host_.run();

    if (self->output_gain_ != 1.0F) {
      self->apply_gain(left_out, right_out, self->output_gain_);
    }
  }

  void ensure_instance(uint32_t rate, uint32_t n_samples) {
    if (ready_ && rate == rate_ && n_samples == n_samples_) {
      return;
    }

    rate_ = rate;
    n_samples_ = n_samples;
    ready_ = host_.create_instance(rate_, n_samples_);
    if (ready_) {
      apply_limiter_preset_to_host(preset_, host_);
    }
  }

  void apply_gain(std::span<float> left, std::span<float> right, const float gain) const {
    for (size_t i = 0; i < left.size(); ++i) {
      left[i] *= gain;
      right[i] *= gain;
    }
  }

  pw_core* core_ = nullptr;
  pw_thread_loop* thread_loop_ = nullptr;
  LimiterPreset preset_;
  Lv2HostCore host_;
  pw_filter* filter_ = nullptr;
  spa_hook listener_{};
  FilterPort* left_in_ = nullptr;
  FilterPort* right_in_ = nullptr;
  FilterPort* probe_left_ = nullptr;
  FilterPort* probe_right_ = nullptr;
  FilterPort* left_out_ = nullptr;
  FilterPort* right_out_ = nullptr;
  uint32_t node_id_ = SPA_ID_INVALID;
  uint32_t rate_ = 0;
  uint32_t n_samples_ = 0;
  bool ready_ = false;
  bool can_get_node_id_ = false;
  pw_filter_state state_ = PW_FILTER_STATE_UNCONNECTED;
  float input_gain_ = static_cast<float>(ee::math::db_to_linear(preset_.input_gain_db));
  float output_gain_ = static_cast<float>(ee::math::db_to_linear(preset_.output_gain_db));
  std::vector<float> scratch_left_;
  std::vector<float> scratch_right_;
  std::vector<float> dummy_left_;
  std::vector<float> dummy_right_;

  const pw_filter_events filter_events_ = {
      .version = 0,
      .destroy = nullptr,
      .state_changed = &LimiterFilterNode::on_filter_state_changed,
      .io_changed = nullptr,
      .param_changed = nullptr,
      .add_buffer = nullptr,
      .remove_buffer = nullptr,
      .process = &LimiterFilterNode::on_process,
      .drained = nullptr,
      .command = nullptr,
  };
};

class PipeWireRouter::ConvolverFilterNode {
 public:
  ConvolverFilterNode(pw_core* core,
                      pw_thread_loop* thread_loop,
                      const ConvolverPreset& preset,
                      const ResolvedKernel& kernel)
      : core_(core), thread_loop_(thread_loop), preset_(preset), kernel_(kernel) {}

  ~ConvolverFilterNode() {
    disconnect();
  }

  auto connect(std::string& error) -> bool {
    std::string host_error;
    if (!host_.load(preset_, kernel_, host_error)) {
      error = host_error;
      return false;
    }

    pw_thread_loop_lock(thread_loop_);
    auto* props = pw_properties_new(nullptr, nullptr);
    pw_properties_set(props, PW_KEY_APP_ID, kAppId);
    pw_properties_set(props, PW_KEY_NODE_NAME, "ee_eq_cli_convolver");
    pw_properties_set(props, PW_KEY_NODE_NICK, "convolver");
    pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, "ee-eq-cli Convolver");
    pw_properties_set(props, PW_KEY_MEDIA_TYPE, "Audio");
    pw_properties_set(props, PW_KEY_MEDIA_CATEGORY, "Duplex");
    pw_properties_set(props, PW_KEY_MEDIA_ROLE, "DSP");
    pw_properties_set(props, PW_KEY_NODE_GROUP, "ee_eq_cli_group");
    pw_properties_set(props, PW_KEY_NODE_PASSIVE, "true");

    filter_ = pw_filter_new(core_, "ee_eq_cli_convolver", props);
    if (filter_ == nullptr) {
      pw_thread_loop_unlock(thread_loop_);
      error = "failed to create PipeWire convolver filter node";
      return false;
    }

    left_in_ = static_cast<FilterPort*>(pw_filter_add_port(filter_, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                                           sizeof(FilterPort), make_port_props("input_FL", "FL"), nullptr,
                                                           0));
    right_in_ = static_cast<FilterPort*>(pw_filter_add_port(filter_, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                                            sizeof(FilterPort), make_port_props("input_FR", "FR"), nullptr,
                                                            0));
    left_out_ = static_cast<FilterPort*>(pw_filter_add_port(filter_, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                                            sizeof(FilterPort), make_port_props("output_FL", "FL"),
                                                            nullptr, 0));
    right_out_ = static_cast<FilterPort*>(pw_filter_add_port(
        filter_, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(FilterPort), make_port_props("output_FR", "FR"),
        nullptr, 0));

    pw_filter_add_listener(filter_, &listener_, &filter_events_, this);

    const auto filter_flags =
        use_rt_convolver_filter() ? static_cast<pw_filter_flags>(PW_FILTER_FLAG_RT_PROCESS) : static_cast<pw_filter_flags>(0);
    if (pw_filter_connect(filter_, filter_flags, nullptr, 0) != 0) {
      pw_filter_destroy(filter_);
      filter_ = nullptr;
      pw_thread_loop_unlock(thread_loop_);
      error = "failed to connect PipeWire convolver filter";
      return false;
    }

    pw_core_sync(core_, PW_ID_CORE, 0);
    pw_thread_loop_wait(thread_loop_);
    pw_thread_loop_unlock(thread_loop_);

    for (int i = 0; i < 5000 && !can_get_node_id_; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      if (state_ == PW_FILTER_STATE_ERROR) {
        error = "Convolver filter entered PipeWire error state";
        return false;
      }
    }
    if (!can_get_node_id_) {
      error = "timed out waiting for convolver filter node to become usable";
      return false;
    }

    pw_thread_loop_lock(thread_loop_);
    node_id_ = pw_filter_get_node_id(filter_);
    pw_core_sync(core_, PW_ID_CORE, 0);
    pw_thread_loop_wait(thread_loop_);
    pw_thread_loop_unlock(thread_loop_);

    return true;
  }

  void disconnect() {
    if (filter_ == nullptr) {
      host_.stop();
      return;
    }
    pw_thread_loop_lock(thread_loop_);
    pw_filter_disconnect(filter_);
    pw_filter_destroy(filter_);
    pw_thread_loop_unlock(thread_loop_);
    filter_ = nullptr;
    node_id_ = SPA_ID_INVALID;
    host_.stop();
  }

  auto node_id() const -> uint32_t { return node_id_; }

 private:
  static void on_filter_state_changed(void* userdata,
                                      [[maybe_unused]] pw_filter_state old_state,
                                      pw_filter_state state,
                                      [[maybe_unused]] const char* error) {
    auto* self = static_cast<ConvolverFilterNode*>(userdata);
    self->state_ = state;
    self->can_get_node_id_ = (state == PW_FILTER_STATE_STREAMING || state == PW_FILTER_STATE_PAUSED);
  }

  static void on_process(void* userdata, spa_io_position* position) {
    auto* self = static_cast<ConvolverFilterNode*>(userdata);
    const auto n_samples = position->clock.duration;
    const auto rate = position->clock.rate.denom;
    if (n_samples == 0 || rate == 0) {
      return;
    }

    auto* in_left = static_cast<float*>(pw_filter_get_dsp_buffer(self->left_in_, n_samples));
    auto* in_right = static_cast<float*>(pw_filter_get_dsp_buffer(self->right_in_, n_samples));
    auto* out_left = static_cast<float*>(pw_filter_get_dsp_buffer(self->left_out_, n_samples));
    auto* out_right = static_cast<float*>(pw_filter_get_dsp_buffer(self->right_out_, n_samples));

    self->dummy_left_.assign(n_samples, 0.0F);
    self->dummy_right_.assign(n_samples, 0.0F);

    std::span<float> left_in = in_left != nullptr ? std::span<float>(in_left, n_samples) : std::span<float>(self->dummy_left_);
    std::span<float> right_in =
        in_right != nullptr ? std::span<float>(in_right, n_samples) : std::span<float>(self->dummy_right_);
    std::span<float> left_out =
        out_left != nullptr ? std::span<float>(out_left, n_samples) : std::span<float>(self->dummy_left_);
    std::span<float> right_out =
        out_right != nullptr ? std::span<float>(out_right, n_samples) : std::span<float>(self->dummy_right_);

    if (self->preset_.bypass) {
      std::ranges::copy(left_in, left_out.begin());
      std::ranges::copy(right_in, right_out.begin());
      return;
    }

    self->input_left_.resize(n_samples);
    self->input_right_.resize(n_samples);
    std::ranges::copy(left_in, self->input_left_.begin());
    std::ranges::copy(right_in, self->input_right_.begin());

    if (self->input_gain_ != 1.0F) {
      self->apply_gain(self->input_left_, self->input_right_, self->input_gain_);
    }

    std::ranges::copy(self->input_left_, left_out.begin());
    std::ranges::copy(self->input_right_, right_out.begin());

    std::string rate_error;
    if (!self->host_.validate_rate(rate, rate_error)) {
      log::warn(rate_error);
      if (self->output_gain_ != 1.0F) {
        self->apply_gain(left_out, right_out, self->output_gain_);
      }
      return;
    }

    if (!self->host_.ensure_ready(n_samples)) {
      if (self->output_gain_ != 1.0F) {
        self->apply_gain(left_out, right_out, self->output_gain_);
      }
      return;
    }

    self->scratch_left_.assign(left_out.begin(), left_out.end());
    self->scratch_right_.assign(right_out.begin(), right_out.end());

    if (!self->host_.process(self->scratch_left_, self->scratch_right_)) {
      if (self->output_gain_ != 1.0F) {
        self->apply_gain(left_out, right_out, self->output_gain_);
      }
      return;
    }

    for (size_t i = 0; i < left_out.size(); ++i) {
      left_out[i] = (self->wet_ * self->scratch_left_[i]) + (self->dry_ * self->input_left_[i]);
      right_out[i] = (self->wet_ * self->scratch_right_[i]) + (self->dry_ * self->input_right_[i]);
    }

    if (self->output_gain_ != 1.0F) {
      self->apply_gain(left_out, right_out, self->output_gain_);
    }
  }

  template <typename SpanL, typename SpanR>
  void apply_gain(SpanL& left, SpanR& right, const float gain) const {
    for (size_t i = 0; i < left.size(); ++i) {
      left[i] *= gain;
      right[i] *= gain;
    }
  }

  pw_core* core_ = nullptr;
  pw_thread_loop* thread_loop_ = nullptr;
  ConvolverPreset preset_;
  ResolvedKernel kernel_;
  ConvolverHost host_;
  pw_filter* filter_ = nullptr;
  spa_hook listener_{};
  FilterPort* left_in_ = nullptr;
  FilterPort* right_in_ = nullptr;
  FilterPort* left_out_ = nullptr;
  FilterPort* right_out_ = nullptr;
  uint32_t node_id_ = SPA_ID_INVALID;
  bool can_get_node_id_ = false;
  pw_filter_state state_ = PW_FILTER_STATE_UNCONNECTED;
  float input_gain_ = static_cast<float>(ee::math::db_to_linear(preset_.input_gain_db));
  float output_gain_ = static_cast<float>(ee::math::db_to_linear(preset_.output_gain_db));
  float dry_ = preset_.dry_db <= ee::math::minimum_db_level ? 0.0F : static_cast<float>(ee::math::db_to_linear(preset_.dry_db));
  float wet_ = preset_.wet_db <= ee::math::minimum_db_level ? 0.0F : static_cast<float>(ee::math::db_to_linear(preset_.wet_db));
  std::vector<float> scratch_left_;
  std::vector<float> scratch_right_;
  std::vector<float> input_left_;
  std::vector<float> input_right_;
  std::vector<float> dummy_left_;
  std::vector<float> dummy_right_;

  const pw_filter_events filter_events_ = {
      .version = 0,
      .destroy = nullptr,
      .state_changed = &ConvolverFilterNode::on_filter_state_changed,
      .io_changed = nullptr,
      .param_changed = nullptr,
      .add_buffer = nullptr,
      .remove_buffer = nullptr,
      .process = &ConvolverFilterNode::on_process,
      .drained = nullptr,
      .command = nullptr,
  };
};

namespace {

void on_registry_global(void* data,
                        uint32_t id,
                        [[maybe_unused]] uint32_t permissions,
                        const char* type,
                        [[maybe_unused]] uint32_t version,
                        const spa_dict* props) {
  static_cast<PipeWireRouter*>(data)->on_registry_global(id, type, props);
}

void on_registry_global_remove(void* data, uint32_t id) {
  static_cast<PipeWireRouter*>(data)->on_registry_global_remove(id);
}

void on_core_done(void* data, uint32_t id, [[maybe_unused]] int seq) {
  if (id == PW_ID_CORE) {
    static_cast<PipeWireRouter*>(data)->on_core_done();
  }
}

void on_core_error(void* data, uint32_t id, [[maybe_unused]] int seq, int res, const char* message) {
  if (id == PW_ID_CORE) {
    static_cast<PipeWireRouter*>(data)->on_core_error(res, message);
  }
}

void on_node_info(void* data, const pw_node_info* info) {
  auto* node = static_cast<PipeWireRouter::NodeData*>(data);
  node->router->on_node_info(node, info);
}

void on_destroy_node_proxy(void* data) {
  auto* node = static_cast<PipeWireRouter::NodeData*>(data);
  node->router->on_node_proxy_destroy(node);
}

void on_removed_node_proxy(void* data) {
  auto* node = static_cast<PipeWireRouter::NodeData*>(data);
  node->router->on_node_proxy_removed(node);
}

int on_metadata_property(void* data, uint32_t id, const char* key, const char* type, const char* value) {
  (void)id;
  (void)type;
  static_cast<PipeWireRouter*>(data)->on_metadata_property(key, value);
  return 0;
}

const pw_core_events kCoreEvents = {.version = PW_VERSION_CORE_EVENTS,
                                    .info = nullptr,
                                    .done = &on_core_done,
                                    .ping = nullptr,
                                    .error = &on_core_error,
                                    .remove_id = nullptr,
                                    .bound_id = nullptr,
                                    .add_mem = nullptr,
                                    .remove_mem = nullptr,
                                    .bound_props = nullptr};

const pw_registry_events kRegistryEvents = {.version = 0,
                                           .global = &on_registry_global,
                                           .global_remove = &on_registry_global_remove};

const pw_node_events kNodeEvents = {.version = PW_VERSION_NODE_EVENTS,
                                    .info = &on_node_info,
                                    .param = nullptr};

const pw_proxy_events kNodeProxyEvents = {.version = 0,
                                          .destroy = &on_destroy_node_proxy,
                                          .bound = nullptr,
                                          .removed = &on_removed_node_proxy,
                                          .done = nullptr,
                                          .error = nullptr,
                                          .bound_props = nullptr};

const pw_metadata_events kMetadataEvents = {.version = PW_VERSION_METADATA_EVENTS, .property = &on_metadata_property};

}  // namespace

PipeWireRouter::PipeWireRouter(ParsedPreset preset, std::string sink_selector)
    : preset_(std::move(preset)), sink_selector_(std::move(sink_selector)) {
  wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
}

PipeWireRouter::~PipeWireRouter() {
  stop();
}

auto PipeWireRouter::start(std::string& error) -> bool {
  std::optional<ResolvedKernel> resolved_convolver;
  if (preset_.convolver.has_value() && convolver_disabled_by_env()) {
    log::warn("convolver disabled by EE_EQ_CLI_DISABLE_CONVOLVER");
    preset_.plugin_order.erase(std::remove(preset_.plugin_order.begin(), preset_.plugin_order.end(), std::string("convolver")),
                               preset_.plugin_order.end());
    preset_.convolver.reset();
  }

  if (preset_.convolver.has_value()) {
    std::string convolver_warning;
    resolved_convolver = resolve_convolver_kernel(*preset_.convolver, convolver_warning);
    if (resolved_convolver.has_value()) {
      log::info(std::format("convolver kernel resolved: {}", resolved_convolver->path));
    } else if (!convolver_warning.empty()) {
      log::warn(convolver_warning);
    }
  }

  if (preset_.plugin_order.empty()) {
    error = "no supported plugin remained applicable after runtime checks";
    return false;
  }

  if (!setup_core(error)) {
    stop();
    return false;
  }

  if (!wait_for_startup_sink(error)) {
    stop();
    return false;
  }

  create_virtual_sink(error);
  if (!error.empty()) {
    stop();
    return false;
  }

  eq_filter_ = std::make_unique<EqFilterNode>(core_, thread_loop_, preset_.equalizer);
  if (!eq_filter_->connect(error)) {
    stop();
    return false;
  }
  log::info("plugin enabled: equalizer");

  if (preset_.convolver.has_value() && resolved_convolver.has_value()) {
    convolver_filter_ = std::make_unique<ConvolverFilterNode>(core_, thread_loop_, *preset_.convolver, *resolved_convolver);
    if (!convolver_filter_->connect(error)) {
      log::warn("convolver skipped: " + error);
      error.clear();
      convolver_filter_.reset();
      preset_.plugin_order.erase(std::remove(preset_.plugin_order.begin(),
                                             preset_.plugin_order.end(),
                                             std::string("convolver")),
                                 preset_.plugin_order.end());
    } else {
      log::info("plugin enabled: convolver");
    }
  } else {
    preset_.plugin_order.erase(std::remove(preset_.plugin_order.begin(),
                                           preset_.plugin_order.end(),
                                           std::string("convolver")),
                               preset_.plugin_order.end());
  }

  if (preset_.limiter.has_value()) {
    limiter_filter_ = std::make_unique<LimiterFilterNode>(core_, thread_loop_, *preset_.limiter);
    if (!limiter_filter_->connect(error)) {
      stop();
      return false;
    }
    log::info("plugin enabled: limiter");
  }

  if (!wait_for_virtual_sink(error) || !wait_for_node_ports(selected_sink_.id, 2, error) ||
      !wait_for_node_ports(virtual_sink_.id, 2, error) || !wait_for_node_ports(eq_filter_->node_id(), 4, error) ||
      (limiter_filter_ != nullptr && !wait_for_node_ports(limiter_filter_->node_id(), 6, error)) ||
      (convolver_filter_ != nullptr && !wait_for_node_ports(convolver_filter_->node_id(), 4, error))) {
    stop();
    return false;
  }

  if (!connect_chain(error)) {
    stop();
    return false;
  }

  patch_existing_streams();
  return true;
}

auto PipeWireRouter::list_sinks(std::string& error) -> std::vector<std::string> {
  std::vector<std::string> result;

  if (!setup_core(error)) {
    stop();
    return result;
  }

  pw_thread_loop_lock(thread_loop_);
  pw_core_sync(core_, PW_ID_CORE, 0);
  pw_thread_loop_wait(thread_loop_);
  pw_core_sync(core_, PW_ID_CORE, 0);
  pw_thread_loop_wait(thread_loop_);
  pw_thread_loop_unlock(thread_loop_);

  for (const auto& node : snapshot_nodes()) {
    if (node.media_class == "Audio/Sink" && node.name != kVirtualSinkName) {
      result.push_back(std::format("{} [serial {}]", node.name, node.serial));
    }
  }

  stop();
  return result;
}

void PipeWireRouter::stop() {
  if (shutting_down_) {
    return;
  }
  shutting_down_ = true;

  clear_patched_streams();
  destroy_links();
  eq_filter_.reset();
  limiter_filter_.reset();
  convolver_filter_.reset();

  if (virtual_sink_proxy_ != nullptr && core_ != nullptr && thread_loop_ != nullptr) {
    pw_thread_loop_lock(thread_loop_);
    pw_proxy_destroy(virtual_sink_proxy_);
    pw_core_sync(core_, PW_ID_CORE, 0);
    pw_thread_loop_wait(thread_loop_);
    pw_thread_loop_unlock(thread_loop_);
    virtual_sink_proxy_ = nullptr;
  }

  for (auto* node : bound_nodes_) {
    if (node != nullptr && node->info != nullptr) {
      delete node->info;
      node->info = nullptr;
    }
    if (node != nullptr &&
        (node->proxy_listener.link.next != nullptr || node->proxy_listener.link.prev != nullptr)) {
      spa_hook_remove(&node->proxy_listener);
    }
    if (node != nullptr && node->proxy != nullptr) {
      pw_thread_loop_lock(thread_loop_);
      pw_proxy_destroy(node->proxy);
      pw_core_sync(core_, PW_ID_CORE, 0);
      pw_thread_loop_wait(thread_loop_);
      pw_thread_loop_unlock(thread_loop_);
    }
  }
  bound_nodes_.clear();

  if (metadata_ != nullptr) {
    pw_thread_loop_lock(thread_loop_);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(metadata_));
    pw_core_sync(core_, PW_ID_CORE, 0);
    pw_thread_loop_wait(thread_loop_);
    pw_thread_loop_unlock(thread_loop_);
    metadata_ = nullptr;
  }

  if (registry_ != nullptr) {
    pw_thread_loop_lock(thread_loop_);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
    pw_core_sync(core_, PW_ID_CORE, 0);
    pw_thread_loop_wait(thread_loop_);
    pw_thread_loop_unlock(thread_loop_);
    registry_ = nullptr;
  }

  if (core_ != nullptr) {
    pw_core_disconnect(core_);
    core_ = nullptr;
  }

  if (context_ != nullptr) {
    pw_context_destroy(context_);
    context_ = nullptr;
  }

  if (thread_loop_ != nullptr) {
    pw_thread_loop_stop(thread_loop_);
    pw_thread_loop_destroy(thread_loop_);
    thread_loop_ = nullptr;
  }

  if (wake_fd_ != -1) {
    close(wake_fd_);
    wake_fd_ = -1;
  }
}

void PipeWireRouter::schedule_task(std::chrono::milliseconds delay, std::function<void()> action) {
  {
    std::scoped_lock lock(task_mutex_);
    scheduled_tasks_.emplace(std::chrono::steady_clock::now() + delay, std::move(action));
  }

  if (wake_fd_ != -1) {
    const uint64_t value = 1;
    const auto bytes_written = write(wake_fd_, &value, sizeof(value));
    if (bytes_written < 0 && errno != EAGAIN && errno != EINTR) {
      log::warn(std::format("failed to signal router control loop: {}", std::strerror(errno)));
    }
  }
}

auto PipeWireRouter::wake_fd() const -> int {
  return wake_fd_;
}

auto PipeWireRouter::next_task_timeout() const -> std::optional<std::chrono::milliseconds> {
  std::scoped_lock lock(task_mutex_);
  if (scheduled_tasks_.empty()) {
    return std::nullopt;
  }

  const auto now = std::chrono::steady_clock::now();
  const auto due_at = scheduled_tasks_.begin()->first;
  if (due_at <= now) {
    return std::chrono::milliseconds(0);
  }

  return std::chrono::duration_cast<std::chrono::milliseconds>(due_at - now);
}

void PipeWireRouter::run_due_tasks() {
  while (true) {
    std::function<void()> task;
    {
      std::scoped_lock lock(task_mutex_);
      if (scheduled_tasks_.empty()) {
        break;
      }

      const auto next = scheduled_tasks_.begin();
      if (next->first > std::chrono::steady_clock::now()) {
        break;
      }

      task = std::move(next->second);
      scheduled_tasks_.erase(next);
    }

    task();
  }
}

auto PipeWireRouter::setup_core(std::string& error) -> bool {
  pw_init(nullptr, nullptr);

  thread_loop_ = pw_thread_loop_new("ee-eq-cli-pipewire", nullptr);
  if (thread_loop_ == nullptr) {
    error = "failed to create PipeWire thread loop";
    return false;
  }
  if (pw_thread_loop_start(thread_loop_) != 0) {
    error = "failed to start PipeWire thread loop";
    return false;
  }

  pw_thread_loop_lock(thread_loop_);
  context_ = pw_context_new(pw_thread_loop_get_loop(thread_loop_), nullptr, 0);
  if (context_ == nullptr) {
    pw_thread_loop_unlock(thread_loop_);
    error = "failed to create PipeWire context";
    return false;
  }

  core_ = pw_context_connect(context_, nullptr, 0);
  if (core_ == nullptr) {
    pw_thread_loop_unlock(thread_loop_);
    error = "failed to connect to PipeWire core";
    return false;
  }

  registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
  if (registry_ == nullptr) {
    pw_thread_loop_unlock(thread_loop_);
    error = "failed to get PipeWire registry";
    return false;
  }

  core_listener_ = std::make_unique<spa_hook>();
  registry_listener_ = std::make_unique<spa_hook>();
  pw_core_add_listener(core_, core_listener_.get(), &kCoreEvents, this);
  pw_registry_add_listener(registry_, registry_listener_.get(), &kRegistryEvents, this);
  pw_core_sync(core_, PW_ID_CORE, 0);
  pw_thread_loop_wait(thread_loop_);
  pw_thread_loop_unlock(thread_loop_);

  return true;
}

auto PipeWireRouter::wait_for_startup_sink(std::string& error) -> bool {
  const auto matches_selector = [&](const NodeInfo& node) {
    const auto selected_sink = snapshot_selected_sink();
    if (sink_selector_.empty()) {
      return !selected_sink.name.empty() && node.name == selected_sink.name;
    }

    const auto* begin = sink_selector_.c_str();
    char* end = nullptr;
    const auto serial = std::strtoull(begin, &end, 10);
    if (end != begin && end != nullptr && *end == '\0') {
      return node.serial == serial;
    }

    return node.name == sink_selector_;
  };

  for (int i = 0; i < 5000; ++i) {
    if (!sink_selector_.empty()) {
      for (const auto& node : snapshot_nodes()) {
        if (matches_selector(node)) {
          {
            std::scoped_lock lock(state_mutex_);
            selected_sink_ = node;
          }
          startup_sink_locked_ = true;
          log::info(std::format("selected sink override: {} (serial {})", node.name, node.serial));
          return true;
        }
      }
    }

    const auto selected_sink = snapshot_selected_sink();
    if (!selected_sink.name.empty() && selected_sink.serial != 0) {
      startup_sink_locked_ = true;
      log::info(std::format("selected startup sink: {} (serial {})", selected_sink.name, selected_sink.serial));
      return true;
    }

    if (!selected_sink.name.empty()) {
      if (auto node = find_node_by_name(selected_sink.name); node.has_value()) {
        {
          std::scoped_lock lock(state_mutex_);
          selected_sink_ = *node;
        }
        startup_sink_locked_ = true;
        return true;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  error = sink_selector_.empty() ? "failed to resolve the current default sink at startup"
                                 : "failed to resolve requested sink override: " + sink_selector_;
  return false;
}

auto PipeWireRouter::wait_for_virtual_sink(std::string& error) -> bool {
  for (int i = 0; i < 5000; ++i) {
    if (auto node = find_node_by_name(kVirtualSinkName); node.has_value()) {
      {
        std::scoped_lock lock(state_mutex_);
        virtual_sink_ = *node;
      }
      log::info(std::format("virtual sink ready: {} (id {}, serial {})", node->name, node->id, node->serial));
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  error = "failed to observe virtual sink node in PipeWire registry";
  return false;
}

auto PipeWireRouter::wait_for_node_ports(uint32_t node_id, uint32_t minimum_ports, std::string& error) -> bool {
  for (int i = 0; i < 5000; ++i) {
    if (count_node_ports(node_id) >= minimum_ports) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  error = std::format("timed out waiting for ports on node {}", node_id);
  return false;
}

void PipeWireRouter::create_virtual_sink(std::string& error) {
  pw_properties* props = pw_properties_new(nullptr, nullptr);
  pw_properties_set(props, PW_KEY_APP_ID, kAppId);
  pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
  pw_properties_set(props, PW_KEY_NODE_NAME, kVirtualSinkName);
  pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, kVirtualSinkDescription);
  pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
  pw_properties_set(props, PW_KEY_NODE_GROUP, "ee_eq_cli_group");
  pw_properties_set(props, "factory.name", "support.null-audio-sink");
  pw_properties_set(props, "audio.position", "FL,FR");
  pw_properties_set(props, "monitor.channel-volumes", "false");
  pw_properties_set(props, "monitor.passthrough", "true");
  pw_properties_set(props, "priority.session", "0");

  pw_thread_loop_lock(thread_loop_);
  virtual_sink_proxy_ =
      static_cast<pw_proxy*>(pw_core_create_object(core_, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0));
  pw_properties_free(props);
  if (virtual_sink_proxy_ == nullptr) {
    pw_thread_loop_unlock(thread_loop_);
    error = "failed to create virtual sink";
    return;
  }
  pw_core_sync(core_, PW_ID_CORE, 0);
  pw_thread_loop_wait(thread_loop_);
  pw_thread_loop_unlock(thread_loop_);
}

auto PipeWireRouter::connect_chain(std::string& error) -> bool {
  uint32_t previous_node_id = snapshot_virtual_sink().id;

  for (const auto& plugin : preset_.plugin_order) {
    if (plugin == "equalizer") {
      if (!link_nodes(previous_node_id, eq_filter_->node_id(), error)) {
        return false;
      }
      previous_node_id = eq_filter_->node_id();
    } else if (plugin == "convolver") {
      if (convolver_filter_ == nullptr) {
        log::warn("convolver requested in preset but no applicable local kernel/runtime is available; skipping");
        continue;
      }
      if (!link_nodes(previous_node_id, convolver_filter_->node_id(), error)) {
        return false;
      }
      previous_node_id = convolver_filter_->node_id();
    } else if (plugin == "limiter") {
      if (limiter_filter_ == nullptr) {
        error = "limiter requested in preset but limiter filter was not initialized";
        return false;
      }
      if (!link_nodes(previous_node_id, limiter_filter_->node_id(), error)) {
        return false;
      }
      previous_node_id = limiter_filter_->node_id();
    }
  }

  return link_nodes(previous_node_id, snapshot_selected_sink().id, error);
}

void PipeWireRouter::patch_existing_streams() {
  std::scoped_lock lock(state_mutex_);
  size_t patched_now = 0;
  for (const auto& node : bound_nodes_) {
    if (node != nullptr && node->info != nullptr && node->info->media_class == "Stream/Output/Audio" &&
        stream_targets_selected_sink(*node->info)) {
      patch_stream(node->info->serial != 0 ? node->info->serial : node->info->id);
      ++patched_now;
    }
  }

  if (patched_now == 0U) {
    log::warn("no matching output streams were patched for sink " + selected_sink_.name +
              "; there may be no active output streams or they may target a different sink");
  } else {
    log::info(std::format("patched {} existing output stream(s)", patched_now));
  }
}

void PipeWireRouter::queue_patch_stream(uint64_t node_serial) {
  {
    std::scoped_lock lock(state_mutex_);
    if (patched_streams_.contains(node_serial) || pending_patch_streams_.contains(node_serial)) {
      return;
    }

    pending_patch_streams_.insert(node_serial);
  }


  schedule_task(
      std::chrono::milliseconds(150),
      [this, node_serial]() {
        {
          std::scoped_lock lock(state_mutex_);
          pending_patch_streams_.erase(node_serial);

          if (shutting_down_) {
            return;
          }

          if (!find_node_by_serial(node_serial).has_value()) {
            log::warn(std::format("queued patch skipped because stream serial {} is no longer present", node_serial));
            return;
          }
        }

        patch_stream(node_serial);
      });
}

void PipeWireRouter::patch_stream(uint64_t node_serial) {
  uint32_t origin_id = 0;
  uint32_t target_id = 0;
  uint64_t target_serial = 0;
  std::optional<NodeInfo> node_snapshot;

  {
    std::scoped_lock lock(state_mutex_);
    node_snapshot = find_node_by_serial(node_serial);
    if (patched_streams_.contains(node_serial)) {
      if (node_snapshot.has_value()) {
        log::info(std::format("stream already patched: {} (id {})", node_snapshot->name, node_snapshot->id));
      }
      return;
    }

    if (!node_snapshot.has_value()) {
      log::warn(std::format("patch skipped because stream serial {} is no longer present", node_serial));
      return;
    }

    if (virtual_sink_.id == 0 || virtual_sink_.serial == 0) {
      log::warn(std::format("cannot patch stream yet, virtual sink is not ready: {} (id {})",
                            node_snapshot->name,
                            node_snapshot->id));
      return;
    }

    if (metadata_ == nullptr) {
      log::warn(std::format("cannot patch stream yet, PipeWire metadata is unavailable: {} (id {})",
                            node_snapshot->name,
                            node_snapshot->id));
      return;
    }

    origin_id = node_snapshot->id;
    target_id = virtual_sink_.id;
    target_serial = virtual_sink_.serial;
    patched_streams_.insert(node_serial);
    recent_patch_attempts_[node_serial] = PatchAttemptInfo{
        .name = node_snapshot->name,
        .patched_at = std::chrono::steady_clock::now(),
    };
  }

  set_metadata_target_node(origin_id, target_id, target_serial);

  log::info(std::format("patched stream: {} (id {}, serial {}) -> {}",
                        node_snapshot->name,
                        node_snapshot->id,
                        node_snapshot->serial,
                        virtual_sink_.name));
}

void PipeWireRouter::clear_patched_streams() {
  std::vector<uint64_t> stream_serials;
  {
    std::scoped_lock lock(state_mutex_);
    stream_serials.assign(patched_streams_.begin(), patched_streams_.end());
    patched_streams_.clear();
    pending_patch_streams_.clear();
    recent_patch_attempts_.clear();
  }
  for (const auto stream_serial : stream_serials) {
    if (auto node = find_node_by_serial(stream_serial); node.has_value()) {
      log::info(std::format("unpatching stream: {} (id {})", node->name, node->id));
      clear_metadata_target_node(node->id);
    }
  }
}

void PipeWireRouter::destroy_links() {
  for (auto* proxy : created_links_) {
    if (proxy == nullptr || core_ == nullptr || thread_loop_ == nullptr) {
      continue;
    }
    pw_thread_loop_lock(thread_loop_);
    pw_proxy_destroy(proxy);
    pw_core_sync(core_, PW_ID_CORE, 0);
    pw_thread_loop_wait(thread_loop_);
    pw_thread_loop_unlock(thread_loop_);
  }
  created_links_.clear();
}

auto PipeWireRouter::stream_targets_selected_sink(const NodeInfo& node) const -> bool {
  std::scoped_lock lock(state_mutex_);
  if (node.media_class != "Stream/Output/Audio") {
    return false;
  }

  if (node.target_object.empty()) {
    return true;
  }

  if (node.target_object == selected_sink_.name || node.target_object == virtual_sink_.name) {
    return true;
  }

  const auto* begin = node.target_object.c_str();
  char* end = nullptr;
  const auto target_serial = std::strtoull(begin, &end, 10);
  if (end != begin && end != nullptr && *end == '\0') {
    return target_serial == selected_sink_.serial || target_serial == virtual_sink_.serial;
  }

  return false;
}

auto PipeWireRouter::find_node_by_name(std::string_view name) -> std::optional<NodeInfo> {
  std::scoped_lock lock(state_mutex_);
  for (const auto& node : bound_nodes_) {
    if (node != nullptr && node->info != nullptr && node->info->name == name) {
      return *node->info;
    }
  }
  return std::nullopt;
}

auto PipeWireRouter::find_node_by_id(uint32_t id) const -> std::optional<NodeInfo> {
  std::scoped_lock lock(state_mutex_);
  for (const auto& node : bound_nodes_) {
    if (node != nullptr && node->info != nullptr && node->info->id == id) {
      return *node->info;
    }
  }
  return std::nullopt;
}

auto PipeWireRouter::find_node_by_serial(uint64_t serial) const -> std::optional<NodeInfo> {
  std::scoped_lock lock(state_mutex_);
  for (const auto& node : bound_nodes_) {
    if (node != nullptr && node->info != nullptr && node->info->serial == serial) {
      return *node->info;
    }
  }
  return std::nullopt;
}

auto PipeWireRouter::snapshot_nodes() const -> std::vector<NodeInfo> {
  std::vector<NodeInfo> nodes;
  std::scoped_lock lock(state_mutex_);
  nodes.reserve(bound_nodes_.size());
  for (const auto& node : bound_nodes_) {
    if (node != nullptr && node->info != nullptr) {
      nodes.push_back(*node->info);
    }
  }
  return nodes;
}

auto PipeWireRouter::snapshot_ports() const -> std::vector<PortInfo> {
  std::scoped_lock lock(state_mutex_);
  return ports_;
}

auto PipeWireRouter::snapshot_selected_sink() const -> NodeInfo {
  std::scoped_lock lock(state_mutex_);
  return selected_sink_;
}

auto PipeWireRouter::snapshot_virtual_sink() const -> NodeInfo {
  std::scoped_lock lock(state_mutex_);
  return virtual_sink_;
}

auto PipeWireRouter::count_node_ports(uint32_t node_id) const -> uint32_t {
  const auto ports = snapshot_ports();
  return static_cast<uint32_t>(std::count_if(ports.begin(), ports.end(),
                                             [node_id](const auto& port) { return port.node_id == node_id; }));
}

auto PipeWireRouter::get_node_ports(uint32_t node_id, const std::string& direction) const -> std::vector<PortInfo> {
  const auto ports = snapshot_ports();
  std::vector<PortInfo> result;
  for (const auto& port : ports) {
    if (port.node_id == node_id && port.direction == direction) {
      result.push_back(port);
    }
  }
  return result;
}

auto PipeWireRouter::link_nodes(uint32_t output_node_id, uint32_t input_node_id, std::string& error) -> bool {
  const auto output_ports = get_node_ports(output_node_id, "out");
  const auto input_ports = get_node_ports(input_node_id, "in");
  if (output_ports.empty() || input_ports.empty()) {
    error = std::format("missing ports for link {} -> {}", output_node_id, input_node_id);
    return false;
  }

  std::vector<std::pair<PortInfo, PortInfo>> matches;
  bool use_audio_channel = true;
  for (const auto& port : output_ports) {
    if (port.audio_channel != "FL" && port.audio_channel != "FR") {
      use_audio_channel = false;
      break;
    }
  }
  for (const auto& port : input_ports) {
    if (port.audio_channel != "FL" && port.audio_channel != "FR") {
      use_audio_channel = false;
      break;
    }
  }

  for (const auto& outp : output_ports) {
    for (const auto& inp : input_ports) {
      const bool match = use_audio_channel ? (outp.audio_channel == inp.audio_channel) : (outp.port_id == inp.port_id);
      if (match) {
        matches.emplace_back(outp, inp);
      }
    }
  }

  if (matches.empty()) {
    error = std::format("failed to match ports for link {} -> {}", output_node_id, input_node_id);
    return false;
  }

  for (const auto& [outp, inp] : matches) {
    pw_properties* props = pw_properties_new(nullptr, nullptr);
    const auto output_node = std::to_string(output_node_id);
    const auto output_port = std::to_string(outp.id);
    const auto input_node = std::to_string(input_node_id);
    const auto input_port = std::to_string(inp.id);
    pw_properties_set(props, PW_KEY_OBJECT_LINGER, "false");
    pw_properties_set(props, PW_KEY_LINK_OUTPUT_NODE, output_node.c_str());
    pw_properties_set(props, PW_KEY_LINK_OUTPUT_PORT, output_port.c_str());
    pw_properties_set(props, PW_KEY_LINK_INPUT_NODE, input_node.c_str());
    pw_properties_set(props, PW_KEY_LINK_INPUT_PORT, input_port.c_str());

    pw_thread_loop_lock(thread_loop_);
    auto* proxy =
        static_cast<pw_proxy*>(pw_core_create_object(core_, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0));
    pw_properties_free(props);
    if (proxy == nullptr) {
      pw_thread_loop_unlock(thread_loop_);
      error = std::format("failed to create PipeWire link {} -> {}", output_node_id, input_node_id);
      return false;
    }
    pw_core_sync(core_, PW_ID_CORE, 0);
    pw_thread_loop_wait(thread_loop_);
    pw_thread_loop_unlock(thread_loop_);
    created_links_.push_back(proxy);
  }

  return true;
}

void PipeWireRouter::set_metadata_target_node(uint32_t origin_id, uint32_t target_id, uint64_t target_serial) {
  if (metadata_ == nullptr) {
    return;
  }
  const auto target_id_string = std::to_string(target_id);
  const auto target_serial_string = std::to_string(target_serial);
  pw_thread_loop_lock(thread_loop_);
  pw_metadata_set_property(metadata_, origin_id, "target.node", "Spa:Id", target_id_string.c_str());
  pw_metadata_set_property(metadata_, origin_id, "target.object", "Spa:Id", target_serial_string.c_str());
  pw_core_sync(core_, PW_ID_CORE, 0);
  pw_thread_loop_wait(thread_loop_);
  pw_thread_loop_unlock(thread_loop_);

  if (auto node = find_node_by_id(origin_id); node.has_value()) {
    log::info(std::format("metadata target set: {} (id {}, serial {}) -> {}",
                          node->name,
                          node->id,
                          node->serial,
                          virtual_sink_.name));
  } else {
    log::info(std::format("metadata target set: stream id {} -> {}", origin_id, virtual_sink_.name));
  }
}

void PipeWireRouter::clear_metadata_target_node(uint32_t origin_id) {
  if (metadata_ == nullptr) {
    return;
  }
  pw_thread_loop_lock(thread_loop_);
  pw_metadata_set_property(metadata_, origin_id, "target.node", nullptr, nullptr);
  pw_metadata_set_property(metadata_, origin_id, "target.object", nullptr, nullptr);
  pw_core_sync(core_, PW_ID_CORE, 0);
  pw_thread_loop_wait(thread_loop_);
  pw_thread_loop_unlock(thread_loop_);
}

void PipeWireRouter::on_registry_global(uint32_t id, const char* type, const spa_dict* props) {
  std::scoped_lock lock(state_mutex_);
  if (id == SPA_ID_INVALID || props == nullptr) {
    return;
  }

  if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
    const auto* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
    if (name != nullptr && std::strcmp(name, "default") == 0) {
      metadata_ = static_cast<pw_metadata*>(pw_registry_bind(registry_, id, type, PW_VERSION_METADATA, 0));
      if (metadata_ != nullptr) {
        metadata_listener_ = std::make_unique<spa_hook>();
        pw_metadata_add_listener(metadata_, metadata_listener_.get(), &kMetadataEvents, this);
      }
    }
    return;
  }

  if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
    PortInfo info{};
    info.id = id;
    if (const auto* v = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL)) {
      info.serial = parse_uint64(v);
    }
    if (const auto* v = spa_dict_lookup(props, PW_KEY_NODE_ID)) {
      info.node_id = parse_uint32(v);
    }
    if (const auto* v = spa_dict_lookup(props, PW_KEY_PORT_ID)) {
      info.port_id = parse_uint32(v);
    }
    if (const auto* v = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION)) {
      info.direction = v;
    }
    if (const auto* v = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL)) {
      info.audio_channel = v;
    }
    if (const auto* v = spa_dict_lookup(props, PW_KEY_PORT_NAME)) {
      info.name = v;
    }
    ports_.push_back(info);
    return;
  }

  if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
    auto* proxy = static_cast<pw_proxy*>(pw_registry_bind(registry_, id, type, PW_VERSION_NODE, sizeof(NodeData)));
    auto* node_data = static_cast<NodeData*>(pw_proxy_get_user_data(proxy));
    node_data->router = this;
    node_data->proxy = proxy;
    node_data->info = new NodeInfo();
    node_data->info->id = id;
    if (const auto* v = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL)) {
      node_data->info->serial = parse_uint64(v);
    }
    if (const auto* v = spa_dict_lookup(props, PW_KEY_NODE_NAME)) {
      node_data->info->name = v;
    }
    if (const auto* v = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS)) {
      node_data->info->media_class = v;
    }
    pw_proxy_add_object_listener(proxy, &node_data->object_listener, &kNodeEvents, node_data);
    pw_proxy_add_listener(proxy, &node_data->proxy_listener, &kNodeProxyEvents, node_data);
    bound_nodes_.push_back(node_data);
  }
}

void PipeWireRouter::on_node_info(NodeData* data, const pw_node_info* info) {
  std::scoped_lock lock(state_mutex_);
  if (shutting_down_ || data == nullptr || info == nullptr) {
    return;
  }

  if (data->removed || data->info == nullptr) {
    return;
  }

  if (const auto* value = spa_dict_lookup(info->props, PW_KEY_NODE_NAME)) {
    data->info->name = value;
  }
  if (const auto* value = spa_dict_lookup(info->props, PW_KEY_MEDIA_CLASS)) {
    data->info->media_class = value;
  }
  if (const auto* value = spa_dict_lookup(info->props, PW_KEY_TARGET_OBJECT)) {
    data->info->target_object = value;
  } else {
    data->info->target_object.clear();
  }

  if (data->info->name == selected_sink_.name && selected_sink_.serial == 0) {
    selected_sink_ = *data->info;
  }
  if (data->info->name == kVirtualSinkName) {
    virtual_sink_ = *data->info;
  }

  if (data->info->media_class == "Stream/Output/Audio") {
    const auto target = data->info->target_object.empty() ? std::string("<default>") : data->info->target_object;
    if (!data->observation_logged || data->last_logged_target != target) {
      log::info(std::format("stream observed: {} (id {}, serial {}, target {})",
                            data->info->name,
                            data->info->id,
                            data->info->serial,
                            target));
      data->last_logged_target = target;
      data->observation_logged = true;
    }
  }

  if (startup_sink_locked_ && data->info->media_class == "Stream/Output/Audio" && stream_targets_selected_sink(*data->info)) {
    const auto node_serial = data->info->serial != 0 ? data->info->serial : data->info->id;
    const auto loop_key = data->info->name;
    const auto now = std::chrono::steady_clock::now();
    if (const auto it = loop_guard_.find(loop_key); it != loop_guard_.end() && now < it->second.suppress_until) {
      log::warn("suppressing repatch for " + data->info->name + " after rapid recreate loop");
      return;
    }

    if (!patched_streams_.contains(node_serial) && !pending_patch_streams_.contains(node_serial)) {
      log::info(std::format("stream eligible for patch: {} (id {}, serial {})",
                            data->info->name,
                            data->info->id,
                            data->info->serial));
    }
    queue_patch_stream(node_serial);
  }
}

void PipeWireRouter::on_node_proxy_destroy(NodeData* data) {
  std::scoped_lock lock(state_mutex_);
  if (data == nullptr) {
    return;
  }

  if (data->proxy_listener.link.next != nullptr || data->proxy_listener.link.prev != nullptr) {
    spa_hook_remove(&data->proxy_listener);
  }

  data->proxy = nullptr;
}

void PipeWireRouter::on_node_proxy_removed(NodeData* data) {
  if (data == nullptr) {
    return;
  }

  pw_proxy* proxy = nullptr;
  {
    std::scoped_lock lock(state_mutex_);
    data->removed = true;

    if (data->info != nullptr) {
      const auto node_serial = data->info->serial != 0 ? data->info->serial : data->info->id;
      patched_streams_.erase(node_serial);
      pending_patch_streams_.erase(node_serial);

      const auto now = std::chrono::steady_clock::now();
      const auto loop_key = data->info->name;
      if (const auto it = recent_patch_attempts_.find(node_serial); it != recent_patch_attempts_.end()) {
        auto& guard = loop_guard_[loop_key];
        if ((now - it->second.patched_at) <= kRapidRemovalWindow) {
          if ((now - guard.last_removed_at) > kRapidRemovalWindow) {
            guard.rapid_removals = 0;
          }
          guard.rapid_removals += 1;
          guard.last_removed_at = now;
          if (guard.rapid_removals >= kLoopSuppressThreshold) {
            guard.suppress_until = now + kLoopSuppressDuration;
            log::warn(std::format("detected rapid recreate loop for {}; suppressing repatch for {} seconds",
                                  data->info->name,
                                  kLoopSuppressDuration.count()));
          }
        } else {
          auto& guard = loop_guard_[loop_key];
          guard.rapid_removals = 0;
          guard.last_removed_at = now;
        }
        recent_patch_attempts_.erase(it);
      }

      log::info(std::format("stream/node removed: {} (id {}, serial {})",
                            data->info->name,
                            data->info->id,
                            data->info->serial));

      delete data->info;
      data->info = nullptr;
    }

    if (data->object_listener.link.next != nullptr || data->object_listener.link.prev != nullptr) {
      spa_hook_remove(&data->object_listener);
    }

    std::erase(bound_nodes_, data);
    proxy = data->proxy;
    data->proxy = nullptr;
  }

  schedule_task(
      std::chrono::milliseconds(0),
      [this, proxy]() {
        // Do not clear per-stream metadata here. PipeWire can recycle node ids
        // immediately across remove/recreate cycles, and stale cleanup on a
        // reused id is riskier than leaving metadata for a vanished stream.
        if (proxy != nullptr && core_ != nullptr && thread_loop_ != nullptr) {
          pw_thread_loop_lock(thread_loop_);
          pw_proxy_destroy(proxy);
          pw_core_sync(core_, PW_ID_CORE, 0);
          pw_thread_loop_wait(thread_loop_);
          pw_thread_loop_unlock(thread_loop_);
        }
      });
}

void PipeWireRouter::on_registry_global_remove(uint32_t id) {
  std::scoped_lock lock(state_mutex_);
  std::erase_if(ports_, [id](const auto& port) { return port.id == id; });
}

void PipeWireRouter::on_metadata_property(const char* key, const char* value) {
  if (key == nullptr || value == nullptr) {
    return;
  }
  if (std::string(key) != "default.audio.sink" || startup_sink_locked_) {
    return;
  }

  try {
    const auto parsed = nlohmann::json::parse(value);
    const auto name = parsed.value("name", std::string{});
    if (!name.empty() && name != kVirtualSinkName) {
      selected_sink_.name = name;
    }
  } catch (...) {
  }
}

void PipeWireRouter::on_core_done() {
  if (thread_loop_ != nullptr) {
    pw_thread_loop_signal(thread_loop_, false);
  }
}

void PipeWireRouter::on_core_error(int res, const char* message) {
  (void)res;
  log::error(std::format("PipeWire core error: {}", message));
  if (thread_loop_ != nullptr) {
    pw_thread_loop_signal(thread_loop_, false);
  }
}

}  // namespace ee
