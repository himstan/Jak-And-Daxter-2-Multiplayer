#include "game/multiplayer/multiplayer_stats.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

#include "enet/enet.h"
#include "gtest/gtest.h"

TEST(MultiplayerStats, UsesElapsedWireRateWindowsAndAccumulatesTotals) {
  ENetHost host = {};
  MultiplayerStats stats;

  host.totalSentData = 100;
  host.totalReceivedData = 200;
  host.totalSentPackets = 10;
  host.totalReceivedPackets = 20;
  stats.calculate_rates(&host, 1000);

  EXPECT_EQ(stats.send_rate_bytes_per_sec, 0u);
  EXPECT_EQ(stats.recv_rate_bytes_per_sec, 0u);
  EXPECT_EQ(stats.send_rate_packets_per_sec, 0u);
  EXPECT_EQ(stats.recv_rate_packets_per_sec, 0u);

  host.totalSentData = 250;
  host.totalReceivedData = 500;
  host.totalSentPackets = 13;
  host.totalReceivedPackets = 25;
  stats.calculate_rates(&host, 2500);

  EXPECT_EQ(stats.send_rate_bytes_per_sec, 100u);
  EXPECT_EQ(stats.recv_rate_bytes_per_sec, 200u);
  EXPECT_EQ(stats.send_rate_packets_per_sec, 2u);
  EXPECT_EQ(stats.recv_rate_packets_per_sec, 3u);
  EXPECT_EQ(stats.wire_total_sent_bytes, 150u);
  EXPECT_EQ(stats.wire_total_recv_bytes, 300u);
  EXPECT_EQ(stats.wire_total_sent_packets, 3u);
  EXPECT_EQ(stats.wire_total_recv_packets, 5u);
}

TEST(MultiplayerStats, HandlesCounterWraparoundWithoutLosingWireTotals) {
  ENetHost host = {};
  MultiplayerStats stats;

  host.totalSentData = (std::numeric_limits<uint32_t>::max)() - 2;
  host.totalReceivedData = (std::numeric_limits<uint32_t>::max)() - 2;
  host.totalSentPackets = (std::numeric_limits<uint32_t>::max)() - 2;
  host.totalReceivedPackets = (std::numeric_limits<uint32_t>::max)() - 2;
  stats.calculate_rates(&host, 100);

  host.totalSentData = 3;
  host.totalReceivedData = 3;
  host.totalSentPackets = 3;
  host.totalReceivedPackets = 3;
  stats.calculate_rates(&host, 1100);

  EXPECT_EQ(stats.wire_total_sent_bytes, 6u);
  EXPECT_EQ(stats.wire_total_recv_bytes, 6u);
  EXPECT_EQ(stats.wire_total_sent_packets, 6u);
  EXPECT_EQ(stats.wire_total_recv_packets, 6u);
  EXPECT_EQ(stats.send_rate_bytes_per_sec, 6u);
  EXPECT_EQ(stats.recv_rate_packets_per_sec, 6u);
}

TEST(MultiplayerStats, WireTotalsRemain64BitAcrossLongSessions) {
  ENetHost host = {};
  MultiplayerStats stats;
  stats.wire_total_sent_bytes = (std::numeric_limits<uint32_t>::max)();
  stats.wire_total_recv_packets = (std::numeric_limits<uint32_t>::max)();

  host.totalSentData = 10;
  host.totalReceivedPackets = 10;
  stats.calculate_rates(&host, 1000);
  host.totalSentData = 20;
  host.totalReceivedPackets = 20;
  stats.calculate_rates(&host, 2000);

  EXPECT_EQ(stats.wire_total_sent_bytes,
            static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)()) + 10u);
  EXPECT_EQ(stats.wire_total_recv_packets,
            static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)()) + 10u);
}

TEST(MultiplayerStats, TracksWidowSyncAndUsesPacketTypeCountBounds) {
  EXPECT_TRUE(multiplayer_stats_valid_packet_type(11));
  EXPECT_TRUE(multiplayer_stats_valid_packet_type(12));
  EXPECT_FALSE(multiplayer_stats_valid_packet_type(13));
  EXPECT_FALSE(multiplayer_stats_valid_packet_type(-1));

  PacketHeader header = {PacketType::WIDOW_SYNC, 17};
  std::array<uint8_t, sizeof(PacketHeader) + 7> packet = {};
  memcpy(packet.data(), &header, sizeof(header));

  MultiplayerStats stats;
  stats.track_sent_bytes(packet.data(), packet.size());
  stats.track_recv_bytes(packet.data(), packet.size());

  const size_t widow_index = static_cast<size_t>(PacketType::WIDOW_SYNC);
  EXPECT_EQ(stats.sent_bytes_by_type[widow_index], packet.size());
  EXPECT_EQ(stats.recv_bytes_by_type[widow_index], packet.size());
  EXPECT_EQ(stats.sent_packets_by_type[widow_index], 1u);
  EXPECT_EQ(stats.recv_packets_by_type[widow_index], 1u);
}

TEST(MultiplayerStats, CalculatesPerTypePacketRatesFromElapsedTime) {
  ENetHost host = {};
  MultiplayerStats stats;
  const size_t type_index = static_cast<size_t>(PacketType::EVENT_GAME);

  host.totalSentData = 0;
  host.totalReceivedData = 0;
  host.totalSentPackets = 0;
  host.totalReceivedPackets = 0;
  stats.calculate_rates(&host, 1000);

  stats.track_sent_packet(PacketType::EVENT_GAME, 100);
  stats.track_sent_packet(PacketType::EVENT_GAME, 100);
  stats.track_recv_packet(PacketType::EVENT_GAME, 100);
  host.totalSentData = 200;
  host.totalReceivedData = 100;
  host.totalSentPackets = 2;
  host.totalReceivedPackets = 1;
  stats.calculate_rates(&host, 2000);

  EXPECT_EQ(stats.send_packet_rate_by_type[type_index], 2u);
  EXPECT_EQ(stats.recv_packet_rate_by_type[type_index], 1u);
  EXPECT_EQ(stats.sent_packets_by_type[type_index], 2u);
  EXPECT_EQ(stats.recv_packets_by_type[type_index], 1u);
}

TEST(MultiplayerStats, PreservesDecimalEnetLossAndSuppressesDefaultRtt) {
  EXPECT_FLOAT_EQ(multiplayer_enet_ratio_to_percent(32768), 50.0f);
  EXPECT_NEAR(multiplayer_enet_ratio_to_percent(1), 100.0f / 65536.0f, 0.00001f);

  ENetPeer peer = {};
  peer.roundTripTime = ENET_PEER_DEFAULT_ROUND_TRIP_TIME;
  peer.roundTripTimeVariance = 0;
  peer.lastReceiveTime = 0;
  EXPECT_FALSE(multiplayer_enet_rtt_sample_valid(peer));

  peer.lastReceiveTime = 1;
  EXPECT_TRUE(multiplayer_enet_rtt_sample_valid(peer));
}
