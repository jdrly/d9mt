#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

namespace dxvk {

  /**
   * \brief FIFO of retired objects that become reusable once a monotonic
   *        fence reaches the value they were retired at
   *
   * Retire values must be non-decreasing, so the front is always the first
   * to become ready. Holds at most \c capacity objects; \c retire refuses
   * more, and the caller then drops the object (frees it the normal way).
   */
  template<typename T>
  class FencedPool {

  public:

    explicit FencedPool(size_t capacity = 0u)
    : m_capacity(capacity) { }

    void setCapacity(size_t capacity) {
      m_capacity = capacity;
      while (m_items.size() > m_capacity)
        m_items.pop_back();
    }

    bool retire(T&& item, uint64_t readyAt) {
      if (m_items.size() >= m_capacity)
        return false;
      m_items.push_back({ std::move(item), readyAt });
      return true;
    }

    bool take(uint64_t fenceValue, T& out) {
      if (m_items.empty() || m_items.front().readyAt > fenceValue)
        return false;
      out = std::move(m_items.front().item);
      m_items.pop_front();
      return true;
    }

    size_t size() const { return m_items.size(); }

  private:

    struct Entry {
      T        item;
      uint64_t readyAt;
    };

    size_t            m_capacity;
    std::deque<Entry> m_items;

  };

}
