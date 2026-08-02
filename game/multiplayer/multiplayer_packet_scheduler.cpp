#include "game/multiplayer/multiplayer_packet_scheduler.h"

#include "game/multiplayer/generated/multiplayer_schema_generated.h"
#include "game/multiplayer/multiplayer_stats.h"

#include <utility>

MultiplayerPacketScheduler::MultiplayerPacketScheduler(size_t max_packets, size_t max_bytes)
    : max_packets_(max_packets), max_bytes_(max_bytes) {}

MultiplayerPacketScheduler::~MultiplayerPacketScheduler() {
  clear();
}

MultiplayerPacketScheduler::Priority MultiplayerPacketScheduler::priority_for(PacketType type) {
  const auto* descriptor = multiplayer::schema::packet_descriptor(static_cast<uint8_t>(type));
  if (descriptor && descriptor->priority <= static_cast<uint8_t>(Priority::Traffic)) {
    return static_cast<Priority>(descriptor->priority);
  }
  return Priority::Snapshot;
}

bool MultiplayerPacketScheduler::is_coalescible(PacketType type, ENetPacketFlag flags) {
  if ((flags & ENET_PACKET_FLAG_RELIABLE) != 0) {
    return false;
  }
  const auto* descriptor = multiplayer::schema::packet_descriptor(static_cast<uint8_t>(type));
  return descriptor && (descriptor->flags & (1u << 1)) != 0;
}

bool MultiplayerPacketScheduler::is_reliable(const QueuedPacket& packet) {
  return (packet.flags & ENET_PACKET_FLAG_RELIABLE) != 0;
}

void MultiplayerPacketScheduler::destroy_packet(QueuedPacket& packet) {
  packet.data.clear();
}

bool MultiplayerPacketScheduler::remove_coalesced_packet(PacketType type) {
  for (auto& queue : queues_) {
    for (auto it = queue.begin(); it != queue.end(); ++it) {
      if (it->type == type && !is_reliable(*it)) {
        queued_byte_count_ -= it->data.size();
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
        queued_byte_count_ -= it->data.size();
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
  if (!packet) {
    return false;
  }
  const bool accepted = enqueue_plain(peer,
                                      channel,
                                      packet->data,
                                      packet->dataLength,
                                      type,
                                      application_size,
                                      flags);
  enet_packet_destroy(packet);
  return accepted;
}

bool MultiplayerPacketScheduler::enqueue_plain(ENetPeer* peer,
                                               int channel,
                                               const void* packet_data,
                                               size_t size,
                                               PacketType type,
                                               size_t application_size,
                                               ENetPacketFlag flags) {
  if (!packet_data || !peer || type >= PacketType::COUNT || size == 0 || size > max_bytes_) {
    return false;
  }

  if (is_coalescible(type, flags)) {
    remove_coalesced_packet(type);
  }

  if (!make_room(size)) {
    return false;
  }

  QueuedPacket queued;
  queued.peer = peer;
  const auto* bytes = static_cast<const uint8_t*>(packet_data);
  queued.data.assign(bytes, bytes + size);
  queued.type = type;
  queued.application_size = application_size;
  queued.channel = channel;
  queued.flags = flags;
  const auto priority = static_cast<size_t>(priority_for(type));
  queued_byte_count_ += queued.data.size();
  ++queued_packet_count_;
  queues_[priority].push_back(std::move(queued));
  return true;
}

size_t MultiplayerPacketScheduler::flush(MultiplayerStats& stats) {
  return flush_plain(stats, [](ENetPeer* peer,
                               int channel,
                               const uint8_t* data,
                               size_t size,
                               PacketType,
                               size_t,
                               ENetPacketFlag flags) {
    if (!peer || peer->state != ENET_PEER_STATE_CONNECTED) {
      return false;
    }
    ENetPacket* packet = enet_packet_create(data, size, flags);
    if (!packet) {
      return false;
    }
    if (enet_peer_send(peer, channel, packet) != 0) {
      enet_packet_destroy(packet);
      return false;
    }
    return true;
  });
}

size_t MultiplayerPacketScheduler::flush(MultiplayerStats& stats, const PacketSender& sender) {
  size_t sent_count = 0;
  for (auto& queue : queues_) {
    while (!queue.empty()) {
      QueuedPacket queued = std::move(queue.front());
      queue.pop_front();
      queued_byte_count_ -= queued.data.size();
      --queued_packet_count_;

      ENetPacket* packet = enet_packet_create(queued.data.data(), queued.data.size(), queued.flags);
      const bool sent = sender && packet && sender(queued.peer, queued.channel, packet);
      if (sent) {
        stats.track_sent_packet(queued.type, queued.application_size);
        ++sent_count;
      } else if (packet) {
        enet_packet_destroy(packet);
      }
    }
  }
  return sent_count;
}

size_t MultiplayerPacketScheduler::flush_plain(MultiplayerStats& stats,
                                               const PlainPacketSender& sender) {
  size_t sent_count = 0;
  for (auto& queue : queues_) {
    while (!queue.empty()) {
      QueuedPacket queued = std::move(queue.front());
      queue.pop_front();
      queued_byte_count_ -= queued.data.size();
      --queued_packet_count_;

      const bool sent = sender && sender(queued.peer,
                                         queued.channel,
                                         queued.data.data(),
                                         queued.data.size(),
                                         queued.type,
                                         queued.application_size,
                                         queued.flags);
      if (sent) {
        stats.track_sent_packet(queued.type, queued.application_size);
        ++sent_count;
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
