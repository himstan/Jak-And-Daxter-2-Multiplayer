#pragma once

#include "game/multiplayer/multiplayer_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

constexpr size_t kMultiplayerMaxDatagramSize = 2048;

struct MultiplayerDatagram {
  std::array<uint8_t, kMultiplayerMaxDatagramSize> bytes = {};
  size_t size = 0;
};

enum class SecurityReceiveKind {
  REJECTED,
  HANDSHAKE,
  GAMEPLAY,
};

struct SecurityReceiveResult {
  SecurityReceiveKind kind = SecurityReceiveKind::REJECTED;
  MultiplayerDatagram plaintext;
  MultiplayerDatagram response;
};

class MultiplayerSecurity {
 public:
  MultiplayerSecurity() = default;
  ~MultiplayerSecurity();

  bool start_host(uint16_t port);
  bool start_client(const std::string& invite, std::string& host, uint16_t& port);
  void reset();

  bool authenticated() const;
  const std::string& invite_token() const;
  std::string invite_for_address(const std::string& address) const;

  bool make_server_hello(MultiplayerDatagram& output) const;
  SecurityReceiveResult receive(int local_role, const uint8_t* data, size_t size);
  bool seal(int local_role,
            PacketType packet_type,
            const void* plaintext,
            size_t plaintext_size,
            MultiplayerDatagram& output);

 private:
  static constexpr size_t kTokenSize = 16;
  static constexpr size_t kSessionIdSize = 16;
  static constexpr size_t kNonceSize = 32;
  static constexpr size_t kKeySize = 32;
  static constexpr size_t kNoncePrefixSize = 16;

  bool initialize_sodium();
  bool parse_invite(const std::string& invite, std::string& host, uint16_t& port);
  void derive_session_keys(int local_role);
  void make_proof(const char* label, std::array<uint8_t, kKeySize>& proof) const;
  bool accept_counter(uint64_t counter);
  void clear_secrets();

  bool m_host = false;
  bool m_authenticated = false;
  uint16_t m_port = 0;
  uint64_t m_send_counter = 0;
  uint64_t m_highest_received_counter = 0;
  std::array<uint64_t, 4> m_replay_window = {};
  std::array<uint8_t, kTokenSize> m_token = {};
  std::array<uint8_t, kSessionIdSize> m_session_id = {};
  std::array<uint8_t, kNonceSize> m_server_nonce = {};
  std::array<uint8_t, kNonceSize> m_client_nonce = {};
  std::array<uint8_t, kKeySize> m_send_key = {};
  std::array<uint8_t, kKeySize> m_receive_key = {};
  std::array<uint8_t, kNoncePrefixSize> m_send_nonce_prefix = {};
  std::array<uint8_t, kNoncePrefixSize> m_receive_nonce_prefix = {};
  std::string m_encoded_token;
};

void mp_secure_clear_string(std::string& value);
