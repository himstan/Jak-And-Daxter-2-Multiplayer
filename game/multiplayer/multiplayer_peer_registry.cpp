#include "game/multiplayer/multiplayer_peer_registry.h"

#include <algorithm>

namespace {
void clear_host_peer_session(HostPeerSession& session) {
  session.security.reset();
  session.peer = nullptr;
  session.player_id = kMPInvalidPlayerId;
  session.handshake_deadline = 0;
  session.last_receive_time = 0;
  session.last_bootstrap_send_time = 0;
  session.authenticated = false;
  session.identity_ready = false;
  session.local_identity_sent = false;
  session.bootstrap_pending = false;
  session.bootstrap_sent_once = false;
}

uint32_t first_free_player_id(const MultiplayerData& data) {
  for (uint32_t player_id = 0; player_id < kMPMaxPlayers; ++player_id) {
    if (player_id == data.host_player_id) {
      continue;
    }
    bool occupied = false;
    for (const auto& session : data.host_peer_sessions) {
      if (session.authenticated && session.player_id == player_id) {
        occupied = true;
        break;
      }
    }
    if (!occupied) {
      return player_id;
    }
  }
  return kMPInvalidPlayerId;
}
}  // namespace

bool multiplayer_valid_player_limit(uint32_t player_limit) {
  return player_limit >= 2 && player_limit <= kMPMaxPlayers;
}

HostPeerSession* multiplayer_host_peer_find(MultiplayerData& data, ENetPeer* peer) {
  if (!peer) {
    return nullptr;
  }
  for (auto& session : data.host_peer_sessions) {
    if (session.peer == peer) {
      return &session;
    }
  }
  return nullptr;
}

const HostPeerSession* multiplayer_host_peer_find(const MultiplayerData& data,
                                                  const ENetPeer* peer) {
  if (!peer) {
    return nullptr;
  }
  for (const auto& session : data.host_peer_sessions) {
    if (session.peer == peer) {
      return &session;
    }
  }
  return nullptr;
}

HostPeerSession* multiplayer_host_peer_for_player_id(MultiplayerData& data, uint32_t player_id) {
  if (player_id >= kMPMaxPlayers) {
    return nullptr;
  }
  for (auto& session : data.host_peer_sessions) {
    if (session.authenticated && session.player_id == player_id) {
      return &session;
    }
  }
  return nullptr;
}

HostPeerSession* multiplayer_host_peer_allocate(MultiplayerData& data,
                                                ENetPeer* peer,
                                                uint32_t current_time) {
  if (!peer || multiplayer_host_peer_find(data, peer)) {
    return nullptr;
  }
  for (auto& session : data.host_peer_sessions) {
    if (!session.peer) {
      clear_host_peer_session(session);
      session.peer = peer;
      session.handshake_deadline = current_time + 5000;
      return &session;
    }
  }
  return nullptr;
}

bool multiplayer_host_peer_authenticate(MultiplayerData& data,
                                        HostPeerSession& session,
                                        uint32_t current_time) {
  if (!session.peer || session.authenticated || !session.security.authenticated() ||
      !multiplayer_host_has_open_player_slot(data)) {
    return false;
  }
  const uint32_t player_id = first_free_player_id(data);
  if (player_id >= kMPMaxPlayers) {
    return false;
  }
  session.player_id = player_id;
  session.authenticated = true;
  session.handshake_deadline = 0;
  session.last_receive_time = current_time;
  data.authenticated_peer_count.fetch_add(1);
  return true;
}

void multiplayer_host_peer_release(MultiplayerData& data, HostPeerSession& session) {
  if (session.authenticated && data.authenticated_peer_count.load() != 0) {
    data.authenticated_peer_count.fetch_sub(1);
  }
  clear_host_peer_session(session);
}

void multiplayer_host_peer_release(MultiplayerData& data, ENetPeer* peer) {
  if (auto* session = multiplayer_host_peer_find(data, peer)) {
    multiplayer_host_peer_release(data, *session);
  }
}

void multiplayer_host_peer_reset_all(MultiplayerData& data) {
  for (auto& session : data.host_peer_sessions) {
    clear_host_peer_session(session);
  }
  data.authenticated_peer_count = 0;
}

size_t multiplayer_host_pending_peer_count(const MultiplayerData& data) {
  return static_cast<size_t>(std::count_if(
      data.host_peer_sessions.begin(), data.host_peer_sessions.end(),
      [](const HostPeerSession& session) { return session.peer && !session.authenticated; }));
}

size_t multiplayer_host_authenticated_peer_count(const MultiplayerData& data) {
  return data.authenticated_peer_count.load();
}

bool multiplayer_host_has_open_player_slot(const MultiplayerData& data) {
  return multiplayer_valid_player_limit(data.session_player_limit) &&
         multiplayer_host_authenticated_peer_count(data) + 1 < data.session_player_limit;
}

MultiplayerSecurity* multiplayer_security_for_peer(MultiplayerData& data, ENetPeer* peer) {
  if (data.session_role == 0) {
    auto* session = multiplayer_host_peer_find(data, peer);
    return session ? &session->security : nullptr;
  }
  return data.session_role == 1 && peer == data.server_peer ? &data.security : nullptr;
}

const MultiplayerSecurity* multiplayer_security_for_peer(const MultiplayerData& data,
                                                         const ENetPeer* peer) {
  if (data.session_role == 0) {
    const auto* session = multiplayer_host_peer_find(data, peer);
    return session ? &session->security : nullptr;
  }
  return data.session_role == 1 && peer == data.server_peer ? &data.security : nullptr;
}

uint32_t multiplayer_authenticated_sender_player_id(const MultiplayerData& data,
                                                    const ENetPeer* peer) {
  if (data.session_role == 0) {
    const auto* session = multiplayer_host_peer_find(data, peer);
    return session && session->authenticated ? session->player_id : kMPInvalidPlayerId;
  }
  return data.session_role == 1 && peer == data.server_peer && data.security.authenticated()
             ? data.host_player_id
             : kMPInvalidPlayerId;
}

bool multiplayer_peer_is_authenticated(const MultiplayerData& data, const ENetPeer* peer) {
  return multiplayer_authenticated_sender_player_id(data, peer) < kMPMaxPlayers;
}

void multiplayer_host_request_bootstrap_for_all(MultiplayerData& data) {
  for (auto& session : data.host_peer_sessions) {
    if (!session.authenticated || !session.identity_ready) {
      continue;
    }
    session.bootstrap_pending = true;
    session.bootstrap_sent_once = false;
    session.last_bootstrap_send_time = 0;
  }
}
