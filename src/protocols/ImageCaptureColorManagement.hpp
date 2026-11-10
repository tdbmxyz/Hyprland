#pragma once

#include "WaylandProtocol.hpp"
#include "../desktop/DesktopTypes.hpp"
#include "../helpers/cm/ColorManagement.hpp"
#include "../helpers/signal/Signal.hpp"
#include "xx-image-capture-color-management-v1.hpp"

class CImageCaptureSource;
namespace Screenshare {
    class CWorkspaceCaptureSource;
}

class CImageCaptureSourceColors {
  public:
    CImageCaptureSourceColors(SP<CXxImageCaptureSourceColorsV1> resource, SP<CImageCaptureSource> source);
    bool good() const;

  private:
    PHLMONITOR                               monitor() const;
    void                                     updateMonitor();
    void                                     updatePreferred();

    SP<CXxImageCaptureSourceColorsV1>        m_resource;
    PHLMONITORREF                            m_monitor;
    PHLWINDOWREF                             m_window;
    SP<Screenshare::CWorkspaceCaptureSource> m_workspace;
    uint64_t                                 m_preferredID = 0;
    bool                                     m_inert       = false;
    struct {
        CHyprSignalListener descriptionChanged;
        CHyprSignalListener monitorDisconnected;
        CHyprSignalListener sourceDestroyed;
        CHyprSignalListener sourceChanged;
    } m_listeners;
};

class CImageCaptureColorManagementProtocol : public IWaylandProtocol {
  public:
    CImageCaptureColorManagementProtocol(const wl_interface* iface, const int& ver, const std::string& name);
    void bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) override;
    void destroyResource(CImageCaptureSourceColors* resource);

  private:
    std::vector<SP<CXxImageCaptureColorManagerV1>> m_managers;
    std::vector<SP<CImageCaptureSourceColors>>     m_sources;
};

namespace PROTO {
    inline UP<CImageCaptureColorManagementProtocol> imageCaptureColorManagement;
}
