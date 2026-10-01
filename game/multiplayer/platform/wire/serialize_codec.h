#pragma once

#include <type_traits>

#include "third-party/serialize/serialize.h"

namespace multiplayer::platform::wire {

template <typename Stream, typename Value>
bool serialize_uint_bits(Stream& stream, Value& value, const int bits) {
  uint32_t wire_value = static_cast<uint32_t>(value);
  if (!stream.SerializeBits(wire_value, bits)) {
    return false;
  }
  if constexpr (Stream::IsReading) {
    value = static_cast<std::remove_cvref_t<Value>>(wire_value);
  }
  return true;
}

namespace serialization {

template <typename Stream>
bool serialize_i16(Stream& stream, int16_t& value) {
  uint32_t bits = static_cast<uint16_t>(value);
  if (!stream.SerializeBits(bits, 16)) {
    return false;
  }
  if constexpr (Stream::IsReading) {
    value = static_cast<int16_t>(bits);
  }
  return true;
}

template <typename Stream>
bool serialize_i32(Stream& stream, int32_t& value) {
  uint32_t bits = static_cast<uint32_t>(value);
  if (!stream.SerializeBits(bits, 32)) {
    return false;
  }
  if constexpr (Stream::IsReading) {
    value = static_cast<int32_t>(bits);
  }
  return true;
}

template <typename Stream>
bool serialize_f32(Stream& stream, float& value) {
  return serialize::serialize_float_internal(stream, value);
}

template <typename Stream, typename ByteT>
bool serialize_raw_bytes(Stream& stream, ByteT* data, const size_t count) {
  if constexpr (Stream::IsReading) {
    return stream.SerializeBytes(reinterpret_cast<uint8_t*>(data), static_cast<int64_t>(count));
  } else {
    return stream.SerializeBytes(reinterpret_cast<const uint8_t*>(data),
                                 static_cast<int64_t>(count));
  }
}

}  // namespace serialization

template <typename Stream>
bool serialize_u8(Stream& stream, uint8_t& value) {
  return serialize_uint_bits(stream, value, 8);
}

template <typename Stream>
bool serialize_u16(Stream& stream, uint16_t& value) {
  return serialize_uint_bits(stream, value, 16);
}

template <typename Stream>
bool serialize_u32(Stream& stream, uint32_t& value) {
  return serialize_uint_bits(stream, value, 32);
}

template <typename Stream>
bool serialize_u64(Stream& stream, uint64_t& value) {
  return serialize::SerializeBits64Const<64>(stream, value);
}

template <typename Stream>
bool serialize_byte_align(Stream& stream) {
  return stream.SerializeAlign();
}

inline constexpr size_t kSerializeReadPaddingBytes = 8;

constexpr size_t serialize_aligned_buffer_size(const size_t bytes) {
  constexpr size_t alignment = 8;
  return ((bytes + alignment - 1) / alignment) * alignment;
}

}  // namespace multiplayer::platform::wire
