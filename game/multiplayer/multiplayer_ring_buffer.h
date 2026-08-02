#pragma once

#include <array>
#include <cstddef>

template <typename T, size_t Capacity>
class MultiplayerRingBuffer {
 public:
  static_assert(Capacity > 0);

  size_t size() const { return m_size; }
  bool empty() const { return m_size == 0; }

  void clear() {
    m_head = 0;
    m_size = 0;
  }

  void push_overwrite(const T& value) {
    if (m_size == Capacity) {
      m_values[m_head] = value;
      m_head = (m_head + 1) % Capacity;
      return;
    }
    m_values[(m_head + m_size) % Capacity] = value;
    ++m_size;
  }

  bool try_push(const T& value) {
    if (m_size == Capacity) {
      return false;
    }
    m_values[(m_head + m_size) % Capacity] = value;
    ++m_size;
    return true;
  }

  bool pop(T& value) {
    if (empty()) {
      return false;
    }
    value = m_values[m_head];
    m_head = (m_head + 1) % Capacity;
    --m_size;
    return true;
  }

 private:
  std::array<T, Capacity> m_values = {};
  size_t m_head = 0;
  size_t m_size = 0;
};
