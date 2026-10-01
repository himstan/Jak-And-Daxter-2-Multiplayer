#pragma once

#include <algorithm>
#include <concepts>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "game/multiplayer/platform/core/message_policy.h"
#include "game/multiplayer/platform/wire/serialize_codec.h"

namespace multiplayer::platform::wire {

struct PacketTag {};

template <typename Derived, auto Type>
struct Packet : PacketTag {
  using derived_type = Derived;
  static constexpr auto kPacketType = Type;
};

template <typename PacketT>
concept SendablePacket =
    std::is_base_of_v<PacketTag, PacketT> && requires(PacketT& packet,
                                                      const PacketT& value,
                                                      serialize::MeasureStream& measure,
                                                      serialize::ReadStream& reader,
                                                      serialize::WriteStream& writer) {
      { validate_packet(value) } -> std::same_as<bool>;
      { serialize_fields(measure, packet) } -> std::same_as<bool>;
      { serialize_fields(reader, packet) } -> std::same_as<bool>;
      { serialize_fields(writer, packet) } -> std::same_as<bool>;
    };

template <SendablePacket PacketT>
bool encode_packet(const PacketT& packet, std::vector<uint8_t>& output) {
  output.clear();
  if (!validate_packet(packet)) {
    return false;
  }

  PacketT writable = packet;
  serialize::MeasureStream measure;
  if (!serialize_fields(measure, writable)) {
    return false;
  }

  const auto measured_bits = measure.GetBitsProcessed();
  if (measured_bits < 0) {
    return false;
  }
  const auto measured_bytes = static_cast<size_t>((measured_bits + 7) / 8);
  const auto buffer_size = std::max<size_t>(8, serialize_aligned_buffer_size(measured_bytes));
  output.assign(buffer_size, 0);

  serialize::WriteStream writer(output.data(), static_cast<int64_t>(output.size()));
  if (!serialize_fields(writer, writable)) {
    output.clear();
    return false;
  }
  writer.Flush();

  const auto encoded_bits = writer.GetBitsProcessed();
  if (encoded_bits < 0 || (encoded_bits % 8) != 0 ||
      static_cast<size_t>(encoded_bits / 8) > output.size()) {
    output.clear();
    return false;
  }
  const auto encoded_size = static_cast<size_t>(encoded_bits / 8);
  if (writer.GetBytesProcessed() != static_cast<int64_t>(encoded_size)) {
    output.clear();
    return false;
  }
  output.resize(encoded_size);
  return true;
}

template <SendablePacket PacketT>
std::optional<std::vector<uint8_t>> encode_packet(const PacketT& packet) {
  std::vector<uint8_t> output;
  if (!encode_packet(packet, output)) {
    return std::nullopt;
  }
  return output;
}

template <SendablePacket PacketT>
std::optional<PacketT> decode_packet(const std::span<const uint8_t> bytes) {
  if (bytes.empty()) {
    return std::nullopt;
  }

  std::vector<uint8_t> padded(bytes.size() + kSerializeReadPaddingBytes, 0);
  serialize::ReadStream reader;
  if (!reader.InitializePadded(padded.data(), static_cast<int64_t>(padded.size()), bytes.data(),
                               static_cast<int64_t>(bytes.size()))) {
    return std::nullopt;
  }

  PacketT packet = {};
  if (!serialize_fields(reader, packet) ||
      reader.GetBitsProcessed() != static_cast<int64_t>(bytes.size() * 8) ||
      !validate_packet(packet)) {
    return std::nullopt;
  }
  return packet;
}

}  // namespace multiplayer::platform::wire
