#pragma once

#include "game/multiplayer/multiplayer_protocol.h"
#include "enet/enet.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <deque>
#include <vector>

struct MultiplayerStats;

class MultiplayerPacketScheduler {
 public:
  using PacketSender = std::function<bool(ENetPeer*, int, ENetPacket*)>;
  using PlainPacketSender =
      std::function<bool(ENetPeer*, int, const uint8_t*, size_t, PacketType, size_t, ENetPacketFlag)>;

  static constexpr size_t kDefaultMaxPackets = 256;
  static constexpr size_t kDefaultMaxBytes = 2 * 1024 * 1024;

  explicit MultiplayerPacketScheduler(size_t max_packets = kDefaultMaxPackets,
                                      size_t max_bytes = kDefaultMaxBytes);
  ~MultiplayerPacketScheduler();

  MultiplayerPacketScheduler(const MultiplayerPacketScheduler&) = delete;
  MultiplayerPacketScheduler& operator=(const MultiplayerPacketScheduler&) = delete;

  // Takes ownership of packet whether it is accepted or rejected.
  bool enqueue(ENetPeer* peer,
               int channel,
               ENetPacket* packet,
               PacketType type,
               size_t application_size,
               ENetPacketFlag flags);

  bool enqueue_plain(ENetPeer* peer,
                     int channel,
                     const void* packet_data,
                     size_t size,
                     PacketType type,
                     size_t application_size,
                     ENetPacketFlag flags);

  size_t flush(MultiplayerStats& stats);
  size_t flush(MultiplayerStats& stats, const PacketSender& sender);
  size_t flush_plain(MultiplayerStats& stats, const PlainPacketSender& sender);
  void clear();

  size_t queued_packet_count() const { return queued_packet_count_; }
  size_t queued_byte_count() const { return queued_byte_count_; }

 private:
  enum class Priority : uint8_t {
    ReliableEvent = 0,
    Bootstrap = 1,
    Snapshot = 2,
    Traffic = 3,
  };

  struct QueuedPacket {
    ENetPeer* peer = nullptr;
    std::vector<uint8_t> data;
    PacketType type = PacketType::COUNT;
    size_t application_size = 0;
    int channel = 0;
    ENetPacketFlag flags = static_cast<ENetPacketFlag>(0);
  };

  static Priority priority_for(PacketType type);
  static bool is_coalescible(PacketType type, ENetPacketFlag flags);
  static bool is_reliable(const QueuedPacket& packet);

  bool make_room(size_t packet_bytes);
  bool drop_oldest_unreliable();
  bool remove_coalesced_packet(PacketType type);
  void destroy_packet(QueuedPacket& packet);

  std::array<std::deque<QueuedPacket>, 4> queues_;
  size_t max_packets_ = kDefaultMaxPackets;
  size_t max_bytes_ = kDefaultMaxBytes;
  size_t queued_packet_count_ = 0;
  size_t queued_byte_count_ = 0;
};
