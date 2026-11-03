#pragma once

// The CS command classes: a lambda (and, for DxvkCsDataCmd, its trailing data) recorded into a
// DxvkCsChunk and run on the CS thread. Split from dxvk_cs.h so they build natively without the
// Metal context (test/native/cs_stats_test.cpp runs them through cs_stats::runCounted).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "dxvk_cs_stats.h"

namespace dxvk {

  class MtlContext;

  class DxvkCsCmd {

  public:

    virtual ~DxvkCsCmd() { }

    DxvkCsCmd* next() const {
      return m_next;
    }

    DxvkCsCmd** chain() {
      return &m_next;
    }

    virtual void exec(MtlContext* ctx) = 0;

    // D9MT_PROFILE only (dxvk_cs_stats.h): the kind given at the emit site, Other if none.
    virtual DxvkCsCmdKind kind() const = 0;

  private:

    DxvkCsCmd* m_next = nullptr;

  };


  template<typename T, DxvkCsCmdKind K = DxvkCsCmdKind::Other>
  class DxvkCsTypedCmd : public DxvkCsCmd {

  public:

    DxvkCsTypedCmd(T&& cmd)
    : m_command(std::move(cmd)) { }

    DxvkCsTypedCmd             (DxvkCsTypedCmd&&) = delete;
    DxvkCsTypedCmd& operator = (DxvkCsTypedCmd&&) = delete;

    void exec(MtlContext* ctx) {
      m_command(ctx);
    }

    DxvkCsCmdKind kind() const {
      return K;
    }

  private:

    T m_command;

  };


  class DxvkCsDataBlock {
    friend class DxvkCsChunk;
  public:

    size_t count() const {
      return m_structCount;
    }

    void* first() {
      return reinterpret_cast<char*>(this) + m_dataOffset;
    }

    void* at(uint32_t idx) {
      return reinterpret_cast<char*>(this) + m_dataOffset + idx * uint32_t(m_structSize);
    }

  private:

    uint32_t m_dataOffset  = 0u;
    uint16_t m_structSize  = 0u;
    uint16_t m_structCount = 0u;

  };


  template<typename T, typename M, DxvkCsCmdKind K = DxvkCsCmdKind::Other>
  class DxvkCsDataCmd : public DxvkCsCmd {

  public:

    DxvkCsDataCmd(T&& cmd)
    : m_command(std::move(cmd)) { }

    ~DxvkCsDataCmd() {
      auto data = reinterpret_cast<M*>(m_data.first());

      for (size_t i = 0; i < m_data.count(); i++)
        data[i].~M();
    }

    DxvkCsDataCmd             (DxvkCsDataCmd&&) = delete;
    DxvkCsDataCmd& operator = (DxvkCsDataCmd&&) = delete;

    void exec(MtlContext* ctx) {
      m_command(ctx, reinterpret_cast<M*>(m_data.first()), m_data.count());
    }

    DxvkCsCmdKind kind() const {
      return K;
    }

    DxvkCsDataBlock* data() {
      return &m_data;
    }

  private:

    alignas(std::max(alignof(T), alignof(M)))
    T               m_command;
    DxvkCsDataBlock m_data;

  };

}
