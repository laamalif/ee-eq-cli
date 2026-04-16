#pragma once

#include <string>

#include "daemon_controller.hpp"

namespace ee {

auto daemon_socket_path(std::string& error) -> std::string;
auto run_daemon_ipc_server(DaemonController& controller, std::string& error, bool watch_signals = true) -> int;
auto send_daemon_request(const DaemonRequest& request, DaemonResponse& response, std::string& error) -> bool;

}  // namespace ee
