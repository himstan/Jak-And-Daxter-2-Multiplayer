#pragma once

#include <cstdint>

namespace multiplayer::platform {

struct NetworkSimulationSettings {
  int32_t send_lag_ms = 0;
  int32_t receive_lag_ms = 0;
  float send_loss_percent = 0.0f;
  float receive_loss_percent = 0.0f;
  float send_jitter_average_ms = 0.0f;
  float send_jitter_maximum_ms = 100.0f;
  float send_jitter_percent = 75.0f;
  float receive_jitter_average_ms = 0.0f;
  float receive_jitter_maximum_ms = 100.0f;
  float receive_jitter_percent = 75.0f;
  float send_reorder_percent = 0.0f;
  float receive_reorder_percent = 0.0f;
  int32_t reorder_delay_ms = 15;
  float send_duplicate_percent = 0.0f;
  float receive_duplicate_percent = 0.0f;
  int32_t duplicate_delay_maximum_ms = 10;
  int32_t send_rate_limit_bytes_per_second = 0;
  int32_t send_rate_limit_burst_bytes = 16 * 1024;
  int32_t receive_rate_limit_bytes_per_second = 0;
  int32_t receive_rate_limit_burst_bytes = 16 * 1024;
};

bool valid_network_simulation_settings(const NetworkSimulationSettings& settings);
bool apply_network_simulation(const NetworkSimulationSettings& settings);

}  // namespace multiplayer::platform
