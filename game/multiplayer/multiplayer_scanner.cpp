#include "multiplayer_scanner.h"
#include "multiplayer_protocol.h"
#include "common/cross_sockets/XSocket.h"
#include "common/log/log.h"
#include <string>
#include <vector>
#include <chrono>

void MultiplayerScanner::start_search(MultiplayerData& data) {
  if (data.join_status == (int)MultiplayerStatus::SEARCHING) return;
  
  data.stop_search = false;
  if (data.scanner_thread.joinable()) {
    data.scanner_thread.join();
  }
  data.scanner_thread = std::thread(scan_thread_func, &data);
}

void MultiplayerScanner::stop_search(MultiplayerData& data) {
  data.stop_search = true;
  data.join_status = (int)MultiplayerStatus::IDLE;
  if (data.scanner_thread.joinable()) {
    data.scanner_thread.join();
  }
}

int MultiplayerScanner::get_status(const MultiplayerData& data) {
  return data.join_status;
}

void MultiplayerScanner::scan_thread_func(MultiplayerData* data) {
  data->join_status = (int)MultiplayerStatus::SEARCHING;
  
  int sock = open_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    data->join_status = (int)MultiplayerStatus::FAILED;
    return;
  }

  // Enable broadcasting
  int broadcast_enable = 1;
  set_socket_option(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));
  set_socket_timeout(sock, 500000); // 500ms timeout

  sockaddr_in broadcast_addr;
  broadcast_addr.sin_family = AF_INET;
  broadcast_addr.sin_port = htons(DISCOVERY_PORT);
  broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;

  lg::info("[Multiplayer] Starting discovery broadcast on port {}...", DISCOVERY_PORT);

  const int max_attempts = 10;
  for (int attempt = 0; attempt < max_attempts && !data->stop_search; ++attempt) {
    // Send discovery ping
    sendto(sock, DISCOVERY_MAGIC, strlen(DISCOVERY_MAGIC), 0, (sockaddr*)&broadcast_addr, sizeof(broadcast_addr));

    // Wait for reply
    char buffer[64];
    sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    
    int bytes_received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&from_addr, &from_len);
    if (bytes_received > 0) {
      buffer[bytes_received] = '\0';
      if (std::string(buffer) == DISCOVERY_MAGIC) {
        data->found_ip = address_to_string(from_addr);
        lg::info("[Multiplayer] Found host at {}", data->found_ip);
        data->join_status = (int)MultiplayerStatus::FOUND;
        close_socket(sock);
        return;
      }
    }
  }

  lg::info("[Multiplayer] Discovery timed out.");
  if (data->join_status == (int)MultiplayerStatus::SEARCHING) {
    data->join_status = (int)MultiplayerStatus::FAILED;
  }
  close_socket(sock);
}
