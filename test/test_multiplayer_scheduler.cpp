#include <cstring>
#include <vector>

#include "enet/enet.h"
#include "game/multiplayer/multiplayer_packet_scheduler.h"
#include "game/multiplayer/multiplayer_stats.h"
#include "gtest/gtest.h"

namespace {

ENetPacket* make_packet(PacketType type, uint32_t sequence) {
  PacketHeader header = {type, sequence};
  return enet_packet_create(&header, sizeof(header), static_cast<ENetPacketFlag>(0));
}

struct SentPacket {
  PacketType type = PacketType::COUNT;
  uint32_t sequence = 0;
};

MultiplayerPacketScheduler::PacketSender recording_sender(std::vector<SentPacket>& sent) {
  return [&sent](ENetPeer*, int, ENetPacket* packet) {
    if (!packet || !packet->data || packet->dataLength < sizeof(PacketHeader)) {
      if (packet) {
        enet_packet_destroy(packet);
      }
      return false;
    }
    PacketHeader header = {};
    memcpy(&header, packet->data, sizeof(header));
    sent.push_back({header.type, header.sequenceNum});
    enet_packet_destroy(packet);
    return true;
  };
}

}  // namespace

TEST(MultiplayerPacketScheduler, HoldsPacketsUntilTheWindowFlushes) {
  MultiplayerPacketScheduler scheduler;
  MultiplayerStats stats;
  std::vector<SentPacket> sent;
  auto* peer = reinterpret_cast<ENetPeer*>(1);

  ASSERT_TRUE(scheduler.enqueue(peer, 0, make_packet(PacketType::EVENT_GAME, 1),
                                PacketType::EVENT_GAME, sizeof(PacketHeader),
                                ENET_PACKET_FLAG_RELIABLE));
  EXPECT_TRUE(sent.empty());
  EXPECT_EQ(stats.sent_packets_by_type[static_cast<size_t>(PacketType::EVENT_GAME)], 0u);

  EXPECT_EQ(scheduler.flush(stats, recording_sender(sent)), 1u);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].type, PacketType::EVENT_GAME);
  EXPECT_EQ(stats.sent_packets_by_type[static_cast<size_t>(PacketType::EVENT_GAME)], 1u);
  EXPECT_EQ(scheduler.queued_packet_count(), 0u);
}

TEST(MultiplayerPacketScheduler, ReliableEventsPrecedeSnapshotsAndKeepFifoOrder) {
  MultiplayerPacketScheduler scheduler;
  MultiplayerStats stats;
  std::vector<SentPacket> sent;
  auto* peer = reinterpret_cast<ENetPeer*>(1);

  ASSERT_TRUE(scheduler.enqueue(peer, 0, make_packet(PacketType::STATE_UPDATE, 10),
                                PacketType::STATE_UPDATE, sizeof(PacketHeader),
                                ENET_PACKET_FLAG_UNSEQUENCED));
  ASSERT_TRUE(scheduler.enqueue(peer, 0, make_packet(PacketType::BOOTSTRAP, 20),
                                PacketType::BOOTSTRAP, sizeof(PacketHeader),
                                ENET_PACKET_FLAG_RELIABLE));
  ASSERT_TRUE(scheduler.enqueue(peer, 0, make_packet(PacketType::EVENT_GAME, 30),
                                PacketType::EVENT_GAME, sizeof(PacketHeader),
                                ENET_PACKET_FLAG_RELIABLE));
  ASSERT_TRUE(scheduler.enqueue(peer, 0, make_packet(PacketType::EVENT_GAME, 31),
                                PacketType::EVENT_GAME, sizeof(PacketHeader),
                                ENET_PACKET_FLAG_RELIABLE));

  ASSERT_EQ(scheduler.flush(stats, recording_sender(sent)), 4u);
  ASSERT_EQ(sent.size(), 4u);
  EXPECT_EQ(sent[0].type, PacketType::EVENT_GAME);
  EXPECT_EQ(sent[0].sequence, 30u);
  EXPECT_EQ(sent[1].type, PacketType::EVENT_GAME);
  EXPECT_EQ(sent[1].sequence, 31u);
  EXPECT_EQ(sent[2].type, PacketType::BOOTSTRAP);
  EXPECT_EQ(sent[3].type, PacketType::STATE_UPDATE);
}

TEST(MultiplayerPacketScheduler, CoalescesOnlyLatestFixedSizeSnapshot) {
  MultiplayerPacketScheduler scheduler;
  MultiplayerStats stats;
  std::vector<SentPacket> sent;
  auto* peer = reinterpret_cast<ENetPeer*>(1);

  ASSERT_TRUE(scheduler.enqueue(peer, 0, make_packet(PacketType::STATE_UPDATE, 1),
                                PacketType::STATE_UPDATE, sizeof(PacketHeader),
                                ENET_PACKET_FLAG_UNSEQUENCED));
  ASSERT_TRUE(scheduler.enqueue(peer, 0, make_packet(PacketType::STATE_UPDATE, 2),
                                PacketType::STATE_UPDATE, sizeof(PacketHeader),
                                ENET_PACKET_FLAG_UNSEQUENCED));
  ASSERT_EQ(scheduler.queued_packet_count(), 1u);

  ASSERT_EQ(scheduler.flush(stats, recording_sender(sent)), 1u);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].sequence, 2u);
}

TEST(MultiplayerPacketScheduler, CoalescesPerTargetPeerAndPlayerStream) {
  MultiplayerPacketScheduler scheduler;
  MultiplayerStats stats;
  std::vector<SentPacket> sent;
  auto* first_peer = reinterpret_cast<ENetPeer*>(1);
  auto* second_peer = reinterpret_cast<ENetPeer*>(2);
  PacketPlayerState state = {};
  state.header.type = PacketType::STATE_UPDATE;

  state.header.sequenceNum = 1;
  state.player_id = 1;
  ASSERT_TRUE(scheduler.enqueue_plain(first_peer, 0, &state, sizeof(state), state.header.type,
                                      sizeof(state), ENET_PACKET_FLAG_UNSEQUENCED));
  state.header.sequenceNum = 2;
  state.player_id = 2;
  ASSERT_TRUE(scheduler.enqueue_plain(first_peer, 0, &state, sizeof(state), state.header.type,
                                      sizeof(state), ENET_PACKET_FLAG_UNSEQUENCED));
  state.header.sequenceNum = 3;
  state.player_id = 1;
  ASSERT_TRUE(scheduler.enqueue_plain(second_peer, 0, &state, sizeof(state), state.header.type,
                                      sizeof(state), ENET_PACKET_FLAG_UNSEQUENCED));
  state.header.sequenceNum = 4;
  state.player_id = 1;
  ASSERT_TRUE(scheduler.enqueue_plain(first_peer, 0, &state, sizeof(state), state.header.type,
                                      sizeof(state), ENET_PACKET_FLAG_UNSEQUENCED));

  EXPECT_EQ(scheduler.queued_packet_count(), 3u);
  EXPECT_EQ(scheduler.flush(stats, recording_sender(sent)), 3u);
}

TEST(MultiplayerPacketScheduler, KeepsRelayedImplicitStreamsIndependent) {
  MultiplayerPacketScheduler scheduler;
  MultiplayerStats stats;
  std::vector<SentPacket> sent;
  auto* peer = reinterpret_cast<ENetPeer*>(1);
  PacketHeader airlock = {PacketType::AIRLOCK_SYNC, 1};

  ASSERT_TRUE(scheduler.enqueue_plain(peer, 0, &airlock, sizeof(airlock), airlock.type,
                                      sizeof(airlock), ENET_PACKET_FLAG_UNSEQUENCED, 1));
  airlock.sequenceNum = 2;
  ASSERT_TRUE(scheduler.enqueue_plain(peer, 0, &airlock, sizeof(airlock), airlock.type,
                                      sizeof(airlock), ENET_PACKET_FLAG_UNSEQUENCED, 2));
  airlock.sequenceNum = 3;
  ASSERT_TRUE(scheduler.enqueue_plain(peer, 0, &airlock, sizeof(airlock), airlock.type,
                                      sizeof(airlock), ENET_PACKET_FLAG_UNSEQUENCED, 1));

  EXPECT_EQ(scheduler.queued_packet_count(), 2u);
  EXPECT_EQ(scheduler.flush(stats, recording_sender(sent)), 2u);
}

TEST(MultiplayerPacketScheduler, PreservesChunkPacketOrder) {
  MultiplayerPacketScheduler scheduler;
  MultiplayerStats stats;
  std::vector<SentPacket> sent;
  auto* peer = reinterpret_cast<ENetPeer*>(1);

  ASSERT_TRUE(scheduler.enqueue(peer, 0, make_packet(PacketType::VEHICLE_SYNC, 100),
                                PacketType::VEHICLE_SYNC, sizeof(PacketHeader),
                                ENET_PACKET_FLAG_UNSEQUENCED));
  ASSERT_TRUE(scheduler.enqueue(peer, 0, make_packet(PacketType::VEHICLE_SYNC, 101),
                                PacketType::VEHICLE_SYNC, sizeof(PacketHeader),
                                ENET_PACKET_FLAG_UNSEQUENCED));

  ASSERT_EQ(scheduler.flush(stats, recording_sender(sent)), 2u);
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0].sequence, 100u);
  EXPECT_EQ(sent[1].sequence, 101u);
}

TEST(MultiplayerPacketScheduler, QueueLimitsDropOldUnreliablePacketsBeforeReliablePackets) {
  MultiplayerPacketScheduler scheduler(2, sizeof(PacketHeader) * 2);
  MultiplayerStats stats;
  std::vector<SentPacket> sent;
  auto* peer = reinterpret_cast<ENetPeer*>(1);

  ASSERT_TRUE(scheduler.enqueue(peer, 0, make_packet(PacketType::VEHICLE_SYNC, 1),
                                PacketType::VEHICLE_SYNC, sizeof(PacketHeader),
                                ENET_PACKET_FLAG_UNSEQUENCED));
  ASSERT_TRUE(scheduler.enqueue(peer, 0, make_packet(PacketType::EVENT_GAME, 2),
                                PacketType::EVENT_GAME, sizeof(PacketHeader),
                                ENET_PACKET_FLAG_RELIABLE));
  ASSERT_TRUE(scheduler.enqueue(peer, 0, make_packet(PacketType::VEHICLE_SYNC, 3),
                                PacketType::VEHICLE_SYNC, sizeof(PacketHeader),
                                ENET_PACKET_FLAG_UNSEQUENCED));

  ASSERT_EQ(scheduler.flush(stats, recording_sender(sent)), 2u);
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0].type, PacketType::EVENT_GAME);
  EXPECT_EQ(sent[1].sequence, 3u);
}

TEST(MultiplayerPacketScheduler, CountsAcceptedPacketsOnly) {
  MultiplayerPacketScheduler scheduler;
  MultiplayerStats stats;
  std::vector<SentPacket> sent;
  auto* peer = reinterpret_cast<ENetPeer*>(1);
  const size_t index = static_cast<size_t>(PacketType::AIRLOCK_SYNC);

  ASSERT_TRUE(scheduler.enqueue(peer, 0, make_packet(PacketType::AIRLOCK_SYNC, 1),
                                PacketType::AIRLOCK_SYNC, 42, ENET_PACKET_FLAG_UNSEQUENCED));
  EXPECT_EQ(stats.sent_packets_by_type[index], 0u);

  EXPECT_EQ(scheduler.flush(stats, [](ENetPeer*, int, ENetPacket* packet) { return false; }), 0u);
  EXPECT_EQ(stats.sent_packets_by_type[index], 0u);

  ASSERT_TRUE(scheduler.enqueue(peer, 0, make_packet(PacketType::AIRLOCK_SYNC, 2),
                                PacketType::AIRLOCK_SYNC, 42, ENET_PACKET_FLAG_UNSEQUENCED));
  EXPECT_EQ(scheduler.flush(stats, recording_sender(sent)), 1u);
  EXPECT_EQ(stats.sent_packets_by_type[index], 1u);
  EXPECT_EQ(stats.sent_bytes_by_type[index], 42u);
}

TEST(MultiplayerPacketScheduler, CoversEveryApplicationPacketTypeAndRejectsCount) {
  MultiplayerPacketScheduler scheduler;
  MultiplayerStats stats;
  std::vector<SentPacket> sent;
  auto* peer = reinterpret_cast<ENetPeer*>(1);

  for (uint8_t value = 0; value < static_cast<uint8_t>(PacketType::COUNT); ++value) {
    const auto type = static_cast<PacketType>(value);
    ASSERT_TRUE(scheduler.enqueue(peer, 0, make_packet(type, value), type, sizeof(PacketHeader),
                                  ENET_PACKET_FLAG_RELIABLE));
  }
  EXPECT_FALSE(scheduler.enqueue(peer, 0, make_packet(PacketType::COUNT, 99), PacketType::COUNT,
                                 sizeof(PacketHeader), ENET_PACKET_FLAG_RELIABLE));
  EXPECT_EQ(scheduler.flush(stats, recording_sender(sent)), static_cast<size_t>(PacketType::COUNT));
  EXPECT_EQ(sent.size(), static_cast<size_t>(PacketType::COUNT));
}
