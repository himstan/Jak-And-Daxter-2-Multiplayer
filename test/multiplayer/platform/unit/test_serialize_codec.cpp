#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <span>
#include <vector>

#include "game/multiplayer/platform/wire/packet_codec.h"
#include "game/multiplayer/platform/wire/serialize_codec.h"
#include "gtest/gtest.h"

namespace platform_packet_test {

struct TestPacket : multiplayer::platform::wire::Packet<TestPacket, uint8_t{7}> {
  uint16_t little_endian = 0;
  int16_t signed_value = 0;
  float float_value = 0.0f;
  uint8_t flags = 0;
};

template <typename Stream>
bool serialize_fields(Stream& stream, TestPacket& packet) {
  if (!multiplayer::platform::wire::serialize_u16(stream, packet.little_endian) ||
      !multiplayer::platform::wire::serialization::serialize_i16(stream, packet.signed_value) ||
      !multiplayer::platform::wire::serialization::serialize_f32(stream, packet.float_value)) {
    return false;
  }

  uint32_t flags = packet.flags;
  if (!stream.SerializeBits(flags, 3)) {
    return false;
  }
  if constexpr (Stream::IsReading) {
    packet.flags = static_cast<uint8_t>(flags);
  }
  return stream.SerializeAlign();
}

bool validate_packet(const TestPacket& packet) {
  return packet.flags <= 7;
}

}  // namespace platform_packet_test

TEST(PlatformSerialization, PrimitiveCodecUsesExplicitLittleEndianAndChecksBounds) {
  std::array<uint8_t, 24> bytes = {};
  serialize::WriteStream writer(bytes.data(), bytes.size());
  uint8_t write_u8 = 0xabu;
  uint16_t write_u16 = 0x1234u;
  uint32_t write_u32 = 0x89abcdefu;
  uint64_t write_u64 = 0x0123456789abcdefull;
  int16_t write_i16 = -2;
  float write_float = 1.0f;
  ASSERT_TRUE(writer.SerializeBits(write_u8, 8));
  ASSERT_TRUE(writer.SerializeBits(write_u16, 16));
  ASSERT_TRUE(writer.SerializeBits(write_u32, 32));
  ASSERT_TRUE(writer.SerializeBits(static_cast<uint32_t>(write_u64), 32));
  ASSERT_TRUE(writer.SerializeBits(static_cast<uint32_t>(write_u64 >> 32), 32));
  ASSERT_TRUE(writer.SerializeBits(static_cast<uint16_t>(write_i16), 16));
  ASSERT_TRUE(multiplayer::platform::wire::serialization::serialize_f32(writer, write_float));
  writer.Flush();
  EXPECT_EQ(writer.GetBytesProcessed(), 21);
  const std::array<uint8_t, 21> expected = {0xab, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x89,
                                            0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23,
                                            0x01, 0xfe, 0xff, 0x00, 0x00, 0x80, 0x3f};
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), bytes.begin()));
  EXPECT_EQ(bytes[21], 0);
  EXPECT_EQ(bytes[22], 0);
  EXPECT_EQ(bytes[23], 0);

  std::array<uint8_t, 21 + multiplayer::platform::wire::kSerializeReadPaddingBytes> padded = {};
  serialize::ReadStream reader;
  ASSERT_TRUE(reader.InitializePadded(padded.data(), padded.size(), bytes.data(), expected.size()));
  uint8_t u8 = 0;
  uint16_t u16 = 0;
  uint32_t u32 = 0;
  uint64_t u64 = 0;
  int16_t i16 = 0;
  float value = 0.0f;
  uint32_t read_u8 = 0;
  uint32_t read_u16 = 0;
  uint32_t read_u32 = 0;
  uint32_t read_u64_low = 0;
  uint32_t read_u64_high = 0;
  uint32_t read_i16 = 0;
  ASSERT_TRUE(reader.SerializeBits(read_u8, 8));
  ASSERT_TRUE(reader.SerializeBits(read_u16, 16));
  ASSERT_TRUE(reader.SerializeBits(read_u32, 32));
  ASSERT_TRUE(reader.SerializeBits(read_u64_low, 32));
  ASSERT_TRUE(reader.SerializeBits(read_u64_high, 32));
  ASSERT_TRUE(reader.SerializeBits(read_i16, 16));
  ASSERT_TRUE(multiplayer::platform::wire::serialization::serialize_f32(reader, value));
  u8 = static_cast<uint8_t>(read_u8);
  u16 = static_cast<uint16_t>(read_u16);
  u32 = read_u32;
  u64 = (static_cast<uint64_t>(read_u64_high) << 32) | read_u64_low;
  i16 = static_cast<int16_t>(read_i16);
  EXPECT_EQ(u8, 0xabu);
  EXPECT_EQ(u16, 0x1234u);
  EXPECT_EQ(u32, 0x89abcdefu);
  EXPECT_EQ(u64, 0x0123456789abcdefull);
  EXPECT_EQ(i16, -2);
  EXPECT_FLOAT_EQ(value, 1.0f);
  EXPECT_EQ(reader.GetBitsProcessed(), static_cast<int64_t>(expected.size() * 8));
  uint32_t too_many = 0;
  EXPECT_FALSE(reader.SerializeBits(too_many, 8));
}

TEST(PlatformSerialization, FloatCodecPreservesRawIEEEBitPatterns) {
  constexpr std::array bit_patterns = {
      0x80000000u,  // -0
      0x7fc12345u,  // NaN with a payload
      0x7f800000u,  // +infinity
      0xff800000u,  // -infinity
  };
  std::array<uint8_t, bit_patterns.size() * sizeof(float)> bytes = {};
  serialize::WriteStream writer(bytes.data(), bytes.size());
  for (const uint32_t bits : bit_patterns) {
    float value = std::bit_cast<float>(bits);
    ASSERT_TRUE(multiplayer::platform::wire::serialization::serialize_f32(writer, value));
  }
  writer.Flush();
  EXPECT_EQ(writer.GetBitsProcessed(), static_cast<int64_t>(bytes.size() * 8));

  std::array<uint8_t, bytes.size() + multiplayer::platform::wire::kSerializeReadPaddingBytes>
      padded = {};
  serialize::ReadStream reader;
  ASSERT_TRUE(reader.InitializePadded(padded.data(), padded.size(), bytes.data(), bytes.size()));
  for (const uint32_t expected_bits : bit_patterns) {
    float value = 0.0f;
    ASSERT_TRUE(multiplayer::platform::wire::serialization::serialize_f32(reader, value));
    EXPECT_EQ(std::bit_cast<uint32_t>(value), expected_bits);
  }
  EXPECT_EQ(reader.GetBitsProcessed(), static_cast<int64_t>(bytes.size() * 8));
}

TEST(PlatformSerialization, ReadStreamRejectsNonZeroAlignmentPadding) {
  std::array<uint8_t, 8> bytes = {};
  serialize::WriteStream writer(bytes.data(), bytes.size());
  ASSERT_TRUE(writer.SerializeBits(1, 1));
  const uint8_t payload = 0xa5;
  ASSERT_TRUE(writer.SerializeBytes(&payload, 1));
  writer.Flush();
  ASSERT_EQ(writer.GetBytesProcessed(), 2);

  bytes[0] |= 0x02;
  std::array<uint8_t, 2 + multiplayer::platform::wire::kSerializeReadPaddingBytes> padded = {};
  serialize::ReadStream reader;
  ASSERT_TRUE(reader.InitializePadded(padded.data(), padded.size(), bytes.data(), 2));
  uint32_t bit = 0;
  ASSERT_TRUE(reader.SerializeBits(bit, 1));
  uint8_t decoded_payload = 0;
  EXPECT_FALSE(reader.SerializeBytes(&decoded_payload, 1));
}

TEST(PlatformSerialization, GenericPacketCodecPreservesFixedWidthFieldsAndBitCount) {
  platform_packet_test::TestPacket input;
  input.little_endian = 0x1234;
  input.signed_value = -2;
  input.float_value = std::bit_cast<float>(0x80000000u);
  input.flags = 5;

  const auto encoded = multiplayer::platform::wire::encode_packet(input);
  ASSERT_TRUE(encoded.has_value());
  ASSERT_EQ(encoded->size(), 9u);
  const std::array<uint8_t, 9> expected = {0x34, 0x12, 0xfe, 0xff, 0, 0, 0, 0x80, 0x05};
  EXPECT_EQ(*encoded, std::vector<uint8_t>(expected.begin(), expected.end()));

  const auto decoded =
      multiplayer::platform::wire::decode_packet<platform_packet_test::TestPacket>(*encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->little_endian, input.little_endian);
  EXPECT_EQ(decoded->signed_value, input.signed_value);
  EXPECT_EQ(std::bit_cast<uint32_t>(decoded->float_value), 0x80000000u);
  EXPECT_EQ(decoded->flags, input.flags);

  serialize::MeasureStream measure;
  auto measured_input = input;
  ASSERT_TRUE(platform_packet_test::serialize_fields(measure, measured_input));
  EXPECT_GE(measure.GetBitsProcessed(), static_cast<int64_t>(encoded->size() * 8));
  EXPECT_EQ((72 + 7) / 8, static_cast<int>(encoded->size()));
}

TEST(PlatformSerialization, GenericPacketCodecPreservesSpecialFloatBitPatterns) {
  for (const uint32_t bits : {0x80000000u, 0x7fc12345u, 0x7f800000u, 0xff800000u}) {
    platform_packet_test::TestPacket input;
    input.float_value = std::bit_cast<float>(bits);
    input.flags = 1;
    const auto encoded = multiplayer::platform::wire::encode_packet(input);
    ASSERT_TRUE(encoded.has_value());
    const auto decoded =
        multiplayer::platform::wire::decode_packet<platform_packet_test::TestPacket>(*encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(std::bit_cast<uint32_t>(decoded->float_value), bits);
  }
}

TEST(PlatformSerialization, GenericPacketCodecRejectsTruncationTrailingBytesAndBadPadding) {
  platform_packet_test::TestPacket input;
  input.flags = 3;
  const auto encoded = multiplayer::platform::wire::encode_packet(input);
  ASSERT_TRUE(encoded.has_value());

  for (size_t size = 0; size < encoded->size(); ++size) {
    EXPECT_FALSE(multiplayer::platform::wire::decode_packet<platform_packet_test::TestPacket>(
                     std::span<const uint8_t>(encoded->data(), size))
                     .has_value());
  }

  auto trailing = *encoded;
  trailing.push_back(0);
  EXPECT_FALSE(
      multiplayer::platform::wire::decode_packet<platform_packet_test::TestPacket>(trailing)
          .has_value());

  auto bad_padding = *encoded;
  bad_padding.back() |= 0x08;
  EXPECT_FALSE(
      multiplayer::platform::wire::decode_packet<platform_packet_test::TestPacket>(bad_padding)
          .has_value());
}

TEST(PlatformSerialization, GenericPacketCodecValidatesBeforeMeasuring) {
  platform_packet_test::TestPacket input;
  input.flags = 8;
  EXPECT_FALSE(multiplayer::platform::wire::encode_packet(input).has_value());
}
