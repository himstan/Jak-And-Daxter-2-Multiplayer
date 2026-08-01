#include "game/multiplayer/multiplayer_packet_scheduler.h"

#include "game/multiplayer/multiplayer_stats.h"

MultiplayerPacketScheduler::MultiplayerPacketScheduler(size_t max_packets, size_t max_bytes)
    : max_packets_(max_packets), max_bytes_(max_bytes) {}

MultiplayerPacketScheduler::~MultiplayerPacketScheduler() {
  clear();
}

MultiplayerPacketScheduler::Priority MultiplayerPacketScheduler::priority_for(PacketType type) {
  switch (type) {
    case PacketType::EVENT_JOIN:
    case PacketType::EVENT_LEAVE:
    case PacketType::EVENT_GAME:
      return Priority::ReliableEvent;
    case PacketType::FULL_SYNC:
      return Priority::FullSync;
    case PacketType::PEDESTRIAN_SYNC:
    case PacketType::VEHICLE_SYNC:
    case PacketType::PALACE_SQUID_SYNC:
    case PacketType::WIDOW_SYNC:
      return Priority::Traffic;
    case PacketType::STATE_UPDATE:
    case PacketType::ENEMY_SYNC:
    case PacketType::TURRET_SYNC:
    case PacketType::AIRLOCK_SYNC:
    case PacketType::COUNT:
      return Priority::Snapshot;
  }
  return Priority::Snapshot;
}

bool MultiplayerPacketScheduler::is_coalescible(PacketType type, ENetPacketFlag flags) {
  if ((flags & ENET_PACKET_FLAG_RELIABLE) != 0) {
    return false;
  }
  switch (type) {
    case PacketType::STATE_UPDATE:
    case PacketType::TURRET_SYNC:
    case PacketType::PALACE_SQUID_SYNC:
    case PacketType::AIRLOCK_SYNC:
    case PacketType::WIDOW_SYNC:
      return true;
    case PacketType::EVENT_JOIN:
    case PacketType::EVENT_LEAVE:
    case PacketType::EVENT_GAME:
    case PacketType::FULL_SYNC:
    case PacketType::ENEMY_SYNC:
    case PacketType::PEDESTRIAN_SYNC:
    case PacketType::VEHICLE_SYNC:
    case PacketType::COUNT:
      return false;
  }
  return false;
}

bool MultiplayerPacketScheduler::is_reliable(const QueuedPacket& packet) {
  return (packet.flags & ENET_PACKET_FLAG_RELIABLE) != 0;
}

void MultiplayerPacketScheduler::destroy_packet(QueuedPacket& packet) {
  if (packet.packet) {
    enet_packet_destroy(packet.packet);
    packet.packet = nullptr;
  }
}

bool MultiplayerPacketScheduler::remove_coalesced_packet(PacketType type) {
  for (auto& queue : queues_) {
    for (auto it = queue.begin(); it != queue.end(); ++it) {
      if (it->type == type && !is_reliable(*it)) {
        queued_byte_count_ -= it->packet ? it->packet->dataLength : 0;
        destroy_packet(*it);
        queue.erase(it);
        --queued_packet_count_;
        return true;
      }
    }
  }
  return false;
}

bool MultiplayerPacketScheduler::drop_oldest_unreliable() {
  for (auto& queue : queues_) {
    for (auto it = queue.begin(); it != queue.end(); ++it) {
      if (!is_reliable(*it)) {
        queued_byte_count_ -= it->packet ? it->packet->dataLength : 0;
        destroy_packet(*it);
        queue.erase(it);
        --queued_packet_count_;
        return true;
      }
    }
  }
  return false;
}

bool MultiplayerPacketScheduler::make_room(size_t packet_bytes) {
  while (true) {
    const size_t available_bytes = queued_byte_count_ < max_bytes_
                                       ? max_bytes_ - queued_byte_count_
                                       : 0;
    if (queued_packet_count_ < max_packets_ && packet_bytes <= available_bytes) {
      return true;
    }
    if (!drop_oldest_unreliable()) {
      return false;
    }
  }
}

bool MultiplayerPacketScheduler::enqueue(ENetPeer* peer,
                                         int channel,
                                         ENetPacket* packet,
                                         PacketType type,
                                         size_t application_size,
                                         ENetPacketFlag flags) {
  if (!packet || !peer || type >= PacketType::COUNT || packet->dataLength > max_bytes_) {
    if (packet) {
      enet_packet_destroy(packet);
    }
    return false;
  }

  if (is_coalescible(type, flags)) {
    remove_coalesced_packet(type);
  }

  if (!make_room(packet->dataLength)) {
    enet_packet_destroy(packet);
    return false;
  }

  QueuedPacket queued;
  queued.peer = peer;
  queued.packet = packet;
  queued.type = type;
  queued.application_size = application_size;
  queued.channel = channel;
  queued.flags = flags;
  const auto priority = static_cast<size_t>(priority_for(type));
  queued_byte_count_ += packet->dataLength;
  ++queued_packet_count_;
  queues_[priority].push_back(queued);
  return true;
}

size_t MultiplayerPacketScheduler::flush(MultiplayerStats& stats) {
  return flush(stats, [](ENetPeer* peer, int channel, ENetPacket* packet) {
    return peer && peer->state == ENET_PEER_STATE_CONNECTED &&
           enet_peer_send(peer, channel, packet) == 0;
  });
}

size_t MultiplayerPacketScheduler::flush(MultiplayerStats& stats, const PacketSender& sender) {
  size_t sent_count = 0;
  for (auto& queue : queues_) {
    while (!queue.empty()) {
      QueuedPacket queued = queue.front();
      queue.pop_front();
      queued_byte_count_ -= queued.packet ? queued.packet->dataLength : 0;
      --queued_packet_count_;

      const bool sent = sender && sender(queued.peer, queued.channel, queued.packet);
      if (sent) {
        stats.track_sent_packet(queued.type, queued.application_size);
        ++sent_count;
      } else {
        destroy_packet(queued);
      }
    }
  }
  return sent_count;
}

void MultiplayerPacketScheduler::clear() {
  for (auto& queue : queues_) {
    for (auto& packet : queue) {
      destroy_packet(packet);
    }
    queue.clear();
  }
  queued_packet_count_ = 0;
  queued_byte_count_ = 0;
}
