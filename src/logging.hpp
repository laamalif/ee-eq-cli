#pragma once

#include <cstdio>
#include <string_view>

namespace ee::log {

inline void info(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stdout);
  std::fputc('\n', stdout);
}

inline void info(const char* message) {
  info(std::string_view(message));
}

inline void warn(std::string_view message) {
  std::fputs("warning: ", stderr);
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
}

inline void warn(const char* message) {
  warn(std::string_view(message));
}

inline void error(std::string_view message) {
  std::fputs("error: ", stderr);
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
}

inline void error(const char* message) {
  error(std::string_view(message));
}

}  // namespace ee::log
