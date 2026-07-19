#pragma once

#include <cstddef>
#include <cstdint>

namespace multiplayer::wire {

inline void store_u16_le(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8);
}

inline void store_u64_le(uint8_t* output, uint64_t value) {
  for (size_t index = 0; index < sizeof(value); ++index) {
    output[index] = static_cast<uint8_t>(value >> (index * 8));
  }
}

inline uint16_t load_u16_le(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) | (static_cast<uint16_t>(input[1]) << 8);
}

inline uint64_t load_u64_le(const uint8_t* input) {
  uint64_t result = 0;
  for (size_t index = 0; index < sizeof(result); ++index) {
    result |= static_cast<uint64_t>(input[index]) << (index * 8);
  }
  return result;
}

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

  bool read_u16(uint16_t& value) {
    if (!can_read(sizeof(value))) {
      return false;
    }
    value = load_u16_le(m_data + m_offset);
    m_offset += sizeof(value);
    return true;
  }

  bool read_u64(uint64_t& value) {
    if (!can_read(sizeof(value))) {
      return false;
    }
    value = load_u64_le(m_data + m_offset);
    m_offset += sizeof(value);
    return true;
  }

  bool consumed_all() const { return m_offset == m_size; }

 private:
  bool can_read(size_t count) const {
    return m_data && count <= m_size && m_offset <= m_size - count;
  }

  const uint8_t* m_data = nullptr;
  size_t m_size = 0;
  size_t m_offset = 0;
};

}  // namespace multiplayer::wire
