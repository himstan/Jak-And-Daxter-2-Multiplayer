#include "game/multiplayer/platform/transport/network_simulation.h"

#include "common/log/log.h"

#include "steam/isteamnetworkingutils.h"
#include "steam/steamnetworkingtypes.h"

namespace multiplayer::platform {
namespace {

bool valid_percent(const float value) {
  return value >= 0.0f && value <= 100.0f;
}

bool valid_range(const int32_t value, const int32_t maximum) {
  return value >= 0 && value <= maximum;
}

bool set_int(const ESteamNetworkingConfigValue key, const int32_t value) {
  return SteamNetworkingUtils()->SetGlobalConfigValueInt32(key, value);
}

bool set_float(const ESteamNetworkingConfigValue key, const float value) {
  return SteamNetworkingUtils()->SetGlobalConfigValueFloat(key, value);
}

}  // namespace

bool valid_network_simulation_settings(const NetworkSimulationSettings& settings) {
  return valid_range(settings.send_lag_ms, 5000) && valid_range(settings.receive_lag_ms, 5000) &&
         valid_percent(settings.send_loss_percent) &&
         valid_percent(settings.receive_loss_percent) && settings.send_jitter_average_ms >= 0.0f &&
         settings.send_jitter_average_ms <= 2000.0f && settings.send_jitter_maximum_ms >= 0.0f &&
         settings.send_jitter_maximum_ms <= 5000.0f &&
         valid_percent(settings.send_jitter_percent) &&
         settings.receive_jitter_average_ms >= 0.0f &&
         settings.receive_jitter_average_ms <= 2000.0f &&
         settings.receive_jitter_maximum_ms >= 0.0f &&
         settings.receive_jitter_maximum_ms <= 5000.0f &&
         valid_percent(settings.receive_jitter_percent) &&
         valid_percent(settings.send_reorder_percent) &&
         valid_percent(settings.receive_reorder_percent) &&
         valid_range(settings.reorder_delay_ms, 5000) &&
         valid_percent(settings.send_duplicate_percent) &&
         valid_percent(settings.receive_duplicate_percent) &&
         valid_range(settings.duplicate_delay_maximum_ms, 5000) &&
         valid_range(settings.send_rate_limit_bytes_per_second, 1024 * 1024 * 1024) &&
         valid_range(settings.send_rate_limit_burst_bytes, 1024 * 1024) &&
         valid_range(settings.receive_rate_limit_bytes_per_second, 1024 * 1024 * 1024) &&
         valid_range(settings.receive_rate_limit_burst_bytes, 1024 * 1024);
}

bool apply_network_simulation(const NetworkSimulationSettings& settings) {
  if (!valid_network_simulation_settings(settings)) {
    lg::error("[Multiplayer] Rejected invalid network simulation settings.");
    return false;
  }

  bool success = true;
  success &= set_int(k_ESteamNetworkingConfig_FakePacketLag_Send, settings.send_lag_ms);
  success &= set_int(k_ESteamNetworkingConfig_FakePacketLag_Recv, settings.receive_lag_ms);
  success &= set_float(k_ESteamNetworkingConfig_FakePacketLoss_Send, settings.send_loss_percent);
  success &= set_float(k_ESteamNetworkingConfig_FakePacketLoss_Recv, settings.receive_loss_percent);
  success &= set_float(k_ESteamNetworkingConfig_FakePacketJitter_Send_Avg,
                       settings.send_jitter_average_ms);
  success &= set_float(k_ESteamNetworkingConfig_FakePacketJitter_Send_Max,
                       settings.send_jitter_maximum_ms);
  success &=
      set_float(k_ESteamNetworkingConfig_FakePacketJitter_Send_Pct, settings.send_jitter_percent);
  success &= set_float(k_ESteamNetworkingConfig_FakePacketJitter_Recv_Avg,
                       settings.receive_jitter_average_ms);
  success &= set_float(k_ESteamNetworkingConfig_FakePacketJitter_Recv_Max,
                       settings.receive_jitter_maximum_ms);
  success &= set_float(k_ESteamNetworkingConfig_FakePacketJitter_Recv_Pct,
                       settings.receive_jitter_percent);
  success &=
      set_float(k_ESteamNetworkingConfig_FakePacketReorder_Send, settings.send_reorder_percent);
  success &=
      set_float(k_ESteamNetworkingConfig_FakePacketReorder_Recv, settings.receive_reorder_percent);
  success &= set_int(k_ESteamNetworkingConfig_FakePacketReorder_Time, settings.reorder_delay_ms);
  success &=
      set_float(k_ESteamNetworkingConfig_FakePacketDup_Send, settings.send_duplicate_percent);
  success &=
      set_float(k_ESteamNetworkingConfig_FakePacketDup_Recv, settings.receive_duplicate_percent);
  success &=
      set_int(k_ESteamNetworkingConfig_FakePacketDup_TimeMax, settings.duplicate_delay_maximum_ms);
  success &= set_int(k_ESteamNetworkingConfig_FakeRateLimit_Send_Rate,
                     settings.send_rate_limit_bytes_per_second);
  success &= set_int(k_ESteamNetworkingConfig_FakeRateLimit_Send_Burst,
                     settings.send_rate_limit_burst_bytes);
  success &= set_int(k_ESteamNetworkingConfig_FakeRateLimit_Recv_Rate,
                     settings.receive_rate_limit_bytes_per_second);
  success &= set_int(k_ESteamNetworkingConfig_FakeRateLimit_Recv_Burst,
                     settings.receive_rate_limit_burst_bytes);
  if (!success)
    lg::error("[Multiplayer] GNS rejected one or more network simulation settings.");
  return success;
}

}  // namespace multiplayer::platform
