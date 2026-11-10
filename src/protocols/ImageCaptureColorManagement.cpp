#include "ImageCaptureColorManagement.hpp"
#include "ColorManagement.hpp"
#include "ImageCaptureSource.hpp"
#include "ImageCopyCapture.hpp"
#include "../desktop/view/window/Window.hpp"
#include "../managers/screenshare/WorkspaceCaptureSource.hpp"
#include "../output/Monitor.hpp"

using namespace NColorManagement;

CImageCaptureSourceColors::CImageCaptureSourceColors(SP<CXxImageCaptureSourceColorsV1> resource, SP<CImageCaptureSource> source) :
    m_resource(std::move(resource)), m_monitor(source->m_monitor), m_window(source->m_window), m_workspace(source->m_workspace) {
    if (!good())
        return;

    m_inert = m_workspace && m_workspace->removed();
    m_resource->setDestroy([this](CXxImageCaptureSourceColorsV1*) { PROTO::imageCaptureColorManagement->destroyResource(this); });
    m_resource->setOnDestroy([this](CXxImageCaptureSourceColorsV1*) { PROTO::imageCaptureColorManagement->destroyResource(this); });
    m_resource->setGetPreferred([this](CXxImageCaptureSourceColorsV1* resource, uint32_t id) {
        const auto MONITOR = m_inert ? nullptr : monitor();
        PROTO::colorManagement->createImageDescriptionReference(resource->client(), id, MONITOR ? MONITOR->m_imageDescription : PImageDescription{});
    });

    if (m_window) {
        m_listeners.sourceDestroyed = m_window->m_events.unmap.listen([this] { m_inert = true; });
        m_listeners.sourceChanged   = m_window->m_events.monitorChanged.listen([this] { updateMonitor(); });
    } else if (m_workspace)
        m_listeners.sourceChanged = m_workspace->m_changed.listen([this] {
            if (m_workspace->removed())
                m_inert = true;
            updateMonitor();
        });

    m_listeners.descriptionChanged = PROTO::colorManagement->m_monitorDescriptionChanged.listen([this](PHLMONITORREF changed) {
        if (monitor() == changed)
            updatePreferred();
    });
    updateMonitor();
}

bool CImageCaptureSourceColors::good() const {
    return m_resource && m_resource->resource();
}

PHLMONITOR CImageCaptureSourceColors::monitor() const {
    if (m_workspace)
        return m_workspace->monitor();
    if (m_window)
        return m_window->m_monitor.lock();
    return m_monitor.lock();
}

void CImageCaptureSourceColors::updateMonitor() {
    m_listeners.monitorDisconnected.reset();
    const auto MONITOR = monitor();
    if (m_inert || !MONITOR)
        return;
    m_listeners.monitorDisconnected = MONITOR->m_events.disconnect.listen([this] { m_inert = true; });
    updatePreferred();
}

void CImageCaptureSourceColors::updatePreferred() {
    const auto MONITOR = monitor();
    if (m_inert || !MONITOR || !MONITOR->m_imageDescription)
        return;
    const auto ID = MONITOR->m_imageDescription->id();
    if (ID == m_preferredID)
        return;
    // An initial event is useful to clients; every subsequent one is a change.
    m_preferredID = ID;
    m_resource->sendPreferredChanged(sc<uint32_t>(ID >> 32), sc<uint32_t>(ID));
}

CImageCaptureColorManagementProtocol::CImageCaptureColorManagementProtocol(const wl_interface* iface, const int& ver, const std::string& name) :
    IWaylandProtocol(iface, ver, name) {
    ;
}

void CImageCaptureColorManagementProtocol::bindManager(wl_client* client, void*, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_managers.emplace_back(makeShared<CXxImageCaptureColorManagerV1>(client, ver, id));
    if (!RESOURCE->resource()) {
        wl_client_post_no_memory(client);
        m_managers.pop_back();
        return;
    }

    const auto destroy = [this](CXxImageCaptureColorManagerV1* resource) { std::erase_if(m_managers, [resource](const auto& item) { return item.get() == resource; }); };
    RESOURCE->setDestroy(destroy);
    RESOURCE->setOnDestroy(destroy);
    RESOURCE->setGetCaptureSourceColors([this](CXxImageCaptureColorManagerV1* resource, uint32_t id, wl_resource* sourceResource) {
        const auto SOURCE = PROTO::imageCaptureSource->sourceFromResource(sourceResource);
        if (!SOURCE) {
            resource->error(0, "invalid capture source");
            return;
        }
        const auto COLORS = m_sources.emplace_back(makeShared<CImageCaptureSourceColors>(makeShared<CXxImageCaptureSourceColorsV1>(resource->client(), 1, id), SOURCE));
        if (!COLORS->good()) {
            resource->noMemory();
            m_sources.pop_back();
        }
    });
    RESOURCE->setSetFrameImageDescription([](CXxImageCaptureColorManagerV1* resource, wl_resource* frame, wl_resource* imageDescription) {
        const auto DESCRIPTION = CColorManagementImageDescription::fromReference(imageDescription);
        if (!DESCRIPTION || !DESCRIPTION->m_settings) {
            resource->error(XX_IMAGE_CAPTURE_COLOR_MANAGER_V1_ERROR_IMAGE_DESCRIPTION_NOT_READY, "image description is not ready");
            return;
        }
        const auto RESULT = PROTO::imageCopyCapture->setFrameImageDescription(frame, DESCRIPTION->m_settings);
        if (RESULT == CImageCopyCaptureProtocol::DESCRIPTION_ALREADY_CAPTURED)
            resource->error(XX_IMAGE_CAPTURE_COLOR_MANAGER_V1_ERROR_ALREADY_CAPTURED, "frame already captured");
        else if (RESULT == CImageCopyCaptureProtocol::DESCRIPTION_INVALID_FRAME)
            resource->error(0, "invalid capture frame");
    });
}

void CImageCaptureColorManagementProtocol::destroyResource(CImageCaptureSourceColors* resource) {
    std::erase_if(m_sources, [resource](const auto& item) { return item.get() == resource; });
}
