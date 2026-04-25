#include "convolver_host.hpp"

#include <sndfile.hh>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ranges>
#include <sched.h>
#include <thread>

#include "app_metadata.hpp"
#include "logging.hpp"
#include "math_utils.hpp"

namespace ee {

namespace {

auto is_power_of_two(uint32_t value) -> bool {
  return value != 0 && (value & (value - 1U)) == 0;
}

void erase_prefix(std::vector<float>& values, size_t count) {
  values.erase(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
}

auto use_realtime_convolver_thread() -> bool {
  if (const char* value = std::getenv(kConvolverSchedFifoEnv); value != nullptr) {
    return *value != '\0' && std::strcmp(value, "0") != 0;
  }
  return false;
}

}  // namespace

ConvolverHost::~ConvolverHost() {
  stop();
}

auto ConvolverHost::load_kernel_file(const ResolvedKernel& kernel, std::string& error) -> KernelData {
  KernelData data;
  SndfileHandle sndfile(kernel.path);
  if (sndfile.error() != 0) {
    error = std::string("failed to open convolver kernel: ") + sndfile.strError();
    return data;
  }

  if (sndfile.frames() == 0 || (sndfile.channels() != 1 && sndfile.channels() != 2 && sndfile.channels() != 4)) {
    error = "only mono, stereo, and true stereo impulse files are supported";
    return data;
  }

  std::vector<float> buffer(static_cast<size_t>(sndfile.frames()) * static_cast<size_t>(sndfile.channels()));
  const auto frames_read = sndfile.readf(buffer.data(), sndfile.frames());
  if (frames_read != sndfile.frames()) {
    error = "failed to read complete convolver kernel file";
    return data;
  }

  data.rate = static_cast<uint32_t>(sndfile.samplerate());
  data.channels = sndfile.channels() == 1 ? 2U : static_cast<uint32_t>(sndfile.channels());
  const auto n_frames = static_cast<size_t>(sndfile.frames());
  data.left.resize(n_frames);
  data.right.resize(n_frames);
  if (data.channels == 4) {
    data.left_right.resize(n_frames);
    data.right_left.resize(n_frames);
  }

  for (size_t i = 0; i < static_cast<size_t>(sndfile.frames()); ++i) {
    if (sndfile.channels() == 1) {
      data.left[i] = buffer[i];
      data.right[i] = buffer[i];
    } else if (sndfile.channels() == 2) {
      data.left[i] = buffer[2 * i];
      data.right[i] = buffer[(2 * i) + 1];
    } else if (sndfile.channels() == 4) {
      data.left[i] = buffer[4 * i];
      data.left_right[i] = buffer[(4 * i) + 1];
      data.right_left[i] = buffer[(4 * i) + 2];
      data.right[i] = buffer[(4 * i) + 3];
    }
  }

  return data;
}

auto ConvolverHost::normalized_zita_block_size(uint32_t stream_block_size) -> uint32_t {
  if (stream_block_size == 0) {
    return 0;
  }

  uint32_t block_size = stream_block_size;
  if (!is_power_of_two(block_size)) {
    block_size = 1;
    while ((block_size << 1U) <= stream_block_size) {
      block_size <<= 1U;
    }
  }

  return std::max(block_size, 64U);
}

void ConvolverHost::normalize_kernel(KernelData& kernel) {
  float power_ll = 0.0F;
  float power_rr = 0.0F;
  float power_lr = 0.0F;
  float power_rl = 0.0F;

  for (size_t i = 0; i < kernel.left.size(); ++i) {
    power_ll += kernel.left[i] * kernel.left[i];
    power_rr += kernel.right[i] * kernel.right[i];
    if (kernel.channels == 4) {
      power_lr += kernel.left_right[i] * kernel.left_right[i];
      power_rl += kernel.right_left[i] * kernel.right_left[i];
    }
  }

  const float power = std::max({power_ll, power_rr, power_lr, power_rl});
  if (power <= 0.0F) {
    return;
  }

  const float gain = std::min(1.0F, 1.0F / std::sqrt(power));
  for (size_t i = 0; i < kernel.left.size(); ++i) {
    kernel.left[i] *= gain;
    kernel.right[i] *= gain;
    if (kernel.channels == 4) {
      kernel.left_right[i] *= gain;
      kernel.right_left[i] *= gain;
    }
  }
}

auto ConvolverHost::load(const ConvolverPreset& preset, const ResolvedKernel& kernel, std::string& error) -> bool {
  std::scoped_lock lock(mutex_);
  preset_ = preset;
  kernel_ = load_kernel_file(kernel, error);
  if (!error.empty()) {
    return false;
  }
  original_kernel_ = kernel_;
  apply_ir_width_and_autogain();
  return true;
}

auto ConvolverHost::validate_rate(uint32_t sample_rate, std::string& error) const -> bool {
  std::scoped_lock lock(mutex_);
  if (kernel_.rate == 0 || kernel_.rate == sample_rate) {
    return true;
  }

  error = std::format("convolver kernel sample rate {} Hz does not match active stream rate {} Hz",
                      kernel_.rate,
                      sample_rate);
  return false;
}

void ConvolverHost::apply_ir_width_and_autogain() {
  kernel_ = original_kernel_;

  const float w = static_cast<float>(preset_.ir_width) * 0.01F;
  const float x = (1.0F - w) / (1.0F + w);

  for (size_t i = 0; i < kernel_.left.size(); ++i) {
    const float ll = kernel_.left[i];
    const float rr = kernel_.right[i];
    float lr = 0.0F;
    float rl = 0.0F;
    if (kernel_.channels == 4) {
      lr = kernel_.left_right[i];
      rl = kernel_.right_left[i];
    }

    kernel_.left[i] = ll + (x * rr);
    kernel_.right[i] = rr + (x * ll);
    if (kernel_.channels == 4) {
      kernel_.left_right[i] = lr - (x * rl);
      kernel_.right_left[i] = rl - (x * lr);
    }
  }

  if (preset_.autogain) {
    normalize_kernel(kernel_);
  }
}

auto ConvolverHost::ensure_ready(uint32_t stream_block_size) -> bool {
  std::scoped_lock lock(mutex_);
  const uint32_t block_size = normalized_zita_block_size(stream_block_size);
  if (stream_block_size == 0 || block_size == 0) {
    return false;
  }

  if (ready_.load(std::memory_order_acquire) && stream_block_size_ == stream_block_size && block_size_ == block_size) {
    return true;
  }

  ready_.store(false, std::memory_order_relaxed);
  if (conv_) {
    conv_->stop_process();
    while (!conv_->check_stop()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  conv_ = std::make_unique<Convproc>();
  stream_block_size_ = stream_block_size;
  block_size_ = block_size;
  conv_->set_options(0);

  if (conv_->configure(2, 2, static_cast<uint32_t>(kernel_.left.size()), block_size, block_size, Convproc::MAXPART, 0.5F) != 0) {
    return false;
  }

  if (conv_->impdata_create(0, 0, 1, kernel_.left.data(), 0, static_cast<int>(kernel_.left.size())) != 0) {
    return false;
  }
  if (conv_->impdata_create(1, 1, 1, kernel_.right.data(), 0, static_cast<int>(kernel_.right.size())) != 0) {
    return false;
  }
  if (kernel_.channels == 4) {
    if (conv_->impdata_create(0, 1, 1, kernel_.left_right.data(), 0, static_cast<int>(kernel_.left_right.size())) != 0) {
      return false;
    }
    if (conv_->impdata_create(1, 0, 1, kernel_.right_left.data(), 0, static_cast<int>(kernel_.right_left.size())) != 0) {
      return false;
    }
  }

  const int sched_policy = use_realtime_convolver_thread() ? SCHED_FIFO : SCHED_OTHER;
  if (conv_->start_process(0, sched_policy) != 0) {
    if (sched_policy == SCHED_FIFO && conv_->start_process(0, SCHED_OTHER) == 0) {
      log::warn("convolver realtime scheduling failed; falling back to normal scheduling");
    } else {
      conv_->cleanup();
      return false;
    }
  }

  pending_input_left_.clear();
  pending_input_right_.clear();
  pending_output_left_.clear();
  pending_output_right_.clear();
  block_left_.assign(block_size_, 0.0F);
  block_right_.assign(block_size_, 0.0F);
  const auto adapter_capacity = static_cast<size_t>(stream_block_size_) + static_cast<size_t>(block_size_);
  pending_input_left_.reserve(adapter_capacity);
  pending_input_right_.reserve(adapter_capacity);
  pending_output_left_.reserve(adapter_capacity);
  pending_output_right_.reserve(adapter_capacity);

  ready_.store(true, std::memory_order_release);
  return true;
}

auto ConvolverHost::process(std::span<float> left, std::span<float> right) -> bool {
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock() || !ready_.load(std::memory_order_relaxed)) {
    return false;
  }

  if (!conv_ || conv_->state() != Convproc::ST_PROC) {
    return false;
  }
  if (left.size() != stream_block_size_ || right.size() != stream_block_size_) {
    return false;
  }
  if (stream_block_size_ != block_size_) {
    return process_buffered(left, right);
  }

  return process_zita_block(left, right);
}

auto ConvolverHost::process_zita_block(std::span<float> left, std::span<float> right) -> bool {
  auto left_in = std::span{conv_->inpdata(0), block_size_};
  auto right_in = std::span{conv_->inpdata(1), block_size_};
  auto left_out = std::span{conv_->outdata(0), block_size_};
  auto right_out = std::span{conv_->outdata(1), block_size_};

  std::ranges::copy(left, left_in.begin());
  std::ranges::copy(right, right_in.begin());

  if (conv_->process(true) != 0) {
    return false;
  }

  std::ranges::copy(left_out, left.begin());
  std::ranges::copy(right_out, right.begin());
  return true;
}

auto ConvolverHost::process_buffered(std::span<float> left, std::span<float> right) -> bool {
  pending_input_left_.insert(pending_input_left_.end(), left.begin(), left.end());
  pending_input_right_.insert(pending_input_right_.end(), right.begin(), right.end());

  while (pending_input_left_.size() >= block_size_) {
    std::ranges::copy_n(pending_input_left_.begin(), block_size_, block_left_.begin());
    std::ranges::copy_n(pending_input_right_.begin(), block_size_, block_right_.begin());

    if (!process_zita_block(block_left_, block_right_)) {
      return false;
    }

    pending_output_left_.insert(pending_output_left_.end(), block_left_.begin(), block_left_.end());
    pending_output_right_.insert(pending_output_right_.end(), block_right_.begin(), block_right_.end());
    erase_prefix(pending_input_left_, block_size_);
    erase_prefix(pending_input_right_, block_size_);
  }

  if (pending_output_left_.size() >= left.size()) {
    std::ranges::copy_n(pending_output_left_.begin(), left.size(), left.begin());
    std::ranges::copy_n(pending_output_right_.begin(), right.size(), right.begin());
    erase_prefix(pending_output_left_, left.size());
    erase_prefix(pending_output_right_, right.size());
    return true;
  }

  const auto offset = left.size() - pending_output_left_.size();
  std::fill(left.begin(), left.begin() + static_cast<std::ptrdiff_t>(offset), 0.0F);
  std::fill(right.begin(), right.begin() + static_cast<std::ptrdiff_t>(offset), 0.0F);
  std::ranges::copy(pending_output_left_, left.begin() + static_cast<std::ptrdiff_t>(offset));
  std::ranges::copy(pending_output_right_, right.begin() + static_cast<std::ptrdiff_t>(offset));
  pending_output_left_.clear();
  pending_output_right_.clear();
  return true;
}

void ConvolverHost::stop() {
  std::scoped_lock lock(mutex_);
  ready_.store(false, std::memory_order_relaxed);
  if (conv_) {
    conv_->stop_process();
    while (!conv_->check_stop()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  pending_input_left_.clear();
  pending_input_right_.clear();
  pending_output_left_.clear();
  pending_output_right_.clear();
}

}  // namespace ee
