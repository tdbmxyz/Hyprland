#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <sys/types.h>

struct wl_proxy;
class CCZwpLinuxDmabufV1;

// Test-only DMA-BUF allocation plus independent high-precision EGL readback.
// The public interface hides GBM/EGL types and all their ownership rules.
class CCaptureGpuBuffer {
  public:
    CCaptureGpuBuffer();
    ~CCaptureGpuBuffer();
    CCaptureGpuBuffer(const CCaptureGpuBuffer&)            = delete;
    CCaptureGpuBuffer& operator=(const CCaptureGpuBuffer&) = delete;

    bool               allocate(dev_t device, uint32_t width, uint32_t height, const std::vector<uint64_t>& modifiers);
    wl_proxy*          createWaylandBuffer(CCZwpLinuxDmabufV1& manager);
    bool               readPixels(std::vector<uint32_t>& pixels);
    uint64_t           modifier() const;

  private:
    struct SImpl;
    std::unique_ptr<SImpl> m_impl;
};
