#include "daemon_backend.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <chrono>
#include <poll.h>
#include <sys/eventfd.h>
#include <system_error>
#include <thread>
#include <unistd.h>

#include "pipewire_router.hpp"

namespace ee {

namespace {

class RealSessionBackend final : public SessionBackend {
 public:
  RealSessionBackend() {
    stop_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  }

  ~RealSessionBackend() override {
    stop_session();
    if (stop_fd_ != -1) {
      close(stop_fd_);
    }
  }

  auto start_session(const ParsedPreset& preset,
                     std::string preset_origin,
                     std::string sink_selector,
                     std::string& error) -> bool override {
    stop_session();

    auto router = std::make_unique<PipeWireRouter>(preset, std::move(sink_selector));
    if (!router->start(error)) {
      return false;
    }

    preset_origin_ = std::move(preset_origin);
    router_ = std::move(router);
    stop_requested_.store(false, std::memory_order_release);
    worker_failed_.store(false, std::memory_order_release);
    if (stop_fd_ != -1) {
      uint64_t ignored = 0;
      while (read(stop_fd_, &ignored, sizeof(ignored)) > 0) {
      }
    }
    worker_ = std::thread([this]() { worker_loop(); });
    return true;
  }

  void stop_session() override {
    stop_requested_.store(true, std::memory_order_release);
    if (stop_fd_ != -1) {
      uint64_t signal = 1;
      [[maybe_unused]] const auto _ = write(stop_fd_, &signal, sizeof(signal));
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    if (router_ != nullptr) {
      router_->stop();
      router_.reset();
    }
    preset_origin_.clear();
  }

  auto list_sinks(std::string& error) -> std::vector<std::string> override {
    PipeWireRouter router(ParsedPreset{}, {});
    return router.list_sinks(error);
  }

  [[nodiscard]] auto snapshot() const -> RuntimeSnapshot override {
    RuntimeSnapshot snapshot;
    if (router_ == nullptr || worker_failed_.load(std::memory_order_acquire)) {
      return snapshot;
    }

    snapshot.session_active = true;
    snapshot.preset_origin = preset_origin_;
    const auto runtime = router_->runtime_snapshot();
    snapshot.sink_name = runtime.sink_name;
    snapshot.sink_serial = runtime.sink_serial;
    snapshot.active_plugins = runtime.active_plugins;
    return snapshot;
  }

 private:
  void worker_loop() {
    while (!stop_requested_.load(std::memory_order_acquire)) {
      if (router_ == nullptr) {
        return;
      }

      std::array<pollfd, 2> poll_fds{{
          {.fd = stop_fd_, .events = POLLIN, .revents = 0},
          {.fd = router_->wake_fd(), .events = POLLIN, .revents = 0},
      }};

      using TimeoutRep = std::chrono::milliseconds::rep;
      const auto next_timeout = router_->next_task_timeout();
      const int timeout_ms =
          next_timeout.has_value() ? static_cast<int>(std::clamp<TimeoutRep>(next_timeout->count(), 0, INT_MAX)) : -1;

      const int poll_result = poll(poll_fds.data(), poll_fds.size(), timeout_ms);
      if (poll_result < 0) {
        if (errno == EINTR) {
          continue;
        }
        worker_failed_.store(true, std::memory_order_release);
        break;
      }

      if (poll_result == 0) {
        router_->run_due_tasks();
        continue;
      }

      if ((poll_fds[0].revents & POLLIN) != 0) {
        uint64_t ignored = 0;
        [[maybe_unused]] const auto _ = read(stop_fd_, &ignored, sizeof(ignored));
        break;
      }

      if ((poll_fds[1].revents & POLLIN) != 0) {
        uint64_t ignored = 0;
        const auto bytes_read = read(router_->wake_fd(), &ignored, sizeof(ignored));
        if (bytes_read < 0 && errno != EAGAIN && errno != EINTR) {
          worker_failed_.store(true, std::memory_order_release);
          break;
        }
      }

      router_->run_due_tasks();
    }
  }

  std::unique_ptr<PipeWireRouter> router_;
  std::string preset_origin_;
  std::thread worker_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> worker_failed_{false};
  int stop_fd_ = -1;
};

}  // namespace

auto make_real_session_backend() -> std::unique_ptr<SessionBackend> {
  return std::make_unique<RealSessionBackend>();
}

}  // namespace ee
