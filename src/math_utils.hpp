#pragma once

#include <cmath>
#include <numbers>

namespace ee::math {

constexpr double minimum_db_level = -100.0;

inline auto db_to_linear(const double db) -> double {
  return std::exp((db / 20.0) * std::numbers::ln10);
}

}  // namespace ee::math
