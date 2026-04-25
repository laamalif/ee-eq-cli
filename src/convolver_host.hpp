#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <zita-convolver.h>

#include "ee_eq_preset_parser.hpp"
#include "kernel_resolver.hpp"

namespace ee {

class ConvolverHost {
 public:
  ConvolverHost() = default;
  ~ConvolverHost();

  ConvolverHost(const ConvolverHost&) = delete;
  auto operator=(const ConvolverHost&) -> ConvolverHost& = delete;

  auto load(const ConvolverPreset& preset, const ResolvedKernel& kernel, std::string& error) -> bool;
  auto validate_rate(uint32_t sample_rate, std::string& error) const -> bool;
  auto ensure_ready(uint32_t stream_block_size) -> bool;
  auto process(std::span<float> left, std::span<float> right) -> bool;
  void stop();

 private:
  struct KernelData {
    uint32_t rate = 0;
    uint32_t channels = 0;
    std::vector<float> left;
    std::vector<float> right;
    std::vector<float> left_right;
    std::vector<float> right_left;
  };

  static auto load_kernel_file(const ResolvedKernel& kernel, std::string& error) -> KernelData;
  static auto normalized_zita_block_size(uint32_t stream_block_size) -> uint32_t;
  static void normalize_kernel(KernelData& kernel);
  void apply_ir_width_and_autogain();
  auto process_zita_block(std::span<float> left, std::span<float> right) -> bool;
  auto process_buffered(std::span<float> left, std::span<float> right) -> bool;

  ConvolverPreset preset_{};
  KernelData kernel_{};
  KernelData original_kernel_{};
  std::unique_ptr<Convproc> conv_;
  uint32_t stream_block_size_ = 0;
  uint32_t block_size_ = 0;
  std::vector<float> pending_input_left_;
  std::vector<float> pending_input_right_;
  std::vector<float> pending_output_left_;
  std::vector<float> pending_output_right_;
  std::vector<float> block_left_;
  std::vector<float> block_right_;
  std::atomic<bool> ready_{false};
  mutable std::mutex mutex_;
};

}  // namespace ee
