#pragma once

#include <thread>

#include "multiplayer_security.h"
#include "multiplayer_types.h"

struct MPDiscoveryResponse {
  uint16_t port = 0;
  std::string room_code;
  uint32_t current_players = 0;
  uint32_t player_limit = 0;
};

class MultiplayerScanner {
 public:
  static void start_search(MultiplayerData& data);
  static bool start_direct_search(MultiplayerData& data,
                                  const std::string& address,
                                  uint16_t game_port);
  static void stop_search(MultiplayerData& data);
  static int get_status(const MultiplayerData& data);

 private:
  static void scan_thread_func(MultiplayerData* data);
};

bool mp_parse_discovery_response(const char* bytes, size_t size, MPDiscoveryResponse& response);
