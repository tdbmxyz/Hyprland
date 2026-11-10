// Diagnostic capture client: writes one explicitly requested full-range BT.2020/PQ
// XBGR2101010 desktop frame. It never infers HDR from the output or buffer depth.
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <limits>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/poll.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland.hpp>
#include <color-management-v1.hpp>
#include <ext-image-capture-source-v1.hpp>
#include <ext-image-copy-capture-v1.hpp>
#include <xx-image-capture-color-management-v1.hpp>
#include <linux-dmabuf-v1.hpp>
#include <drm_fourcc.h>
#include "capture-test/GpuBuffer.hpp"
#include <hyprutils/memory/Casts.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/os/FileDescriptor.hpp>

using namespace Hyprutils::Memory;

struct SOutput {
    CSharedPointer<CCWlOutput> resource;
    std::string                name;
};

class CMapping {
  public:
    ~CMapping() {
        if (m_data != MAP_FAILED)
            munmap(m_data, m_size);
    }
    bool map(int fd, size_t size) {
        m_size = size;
        m_data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        return m_data != MAP_FAILED;
    }
    const char* data() const {
        return sc<const char*>(m_data);
    }

  private:
    void*  m_data = MAP_FAILED;
    size_t m_size = 0;
};

enum eCaptureColor : uint8_t {
    COLOR_PQ,
    COLOR_SRGB,
    COLOR_UNTAGGED
};

struct SOptions {
    bool          dma           = false;
    bool          cursor        = true;
    bool          discardPixels = false;
    uint32_t      frames        = 1;
    eCaptureColor color         = COLOR_PQ;
};

struct SState {
    SOptions                                               options;
    CSharedPointer<CCZwpLinuxDmabufV1>                     linuxDma;
    dev_t                                                  device = 0;
    std::vector<uint64_t>                                  modifiers;
    wl_display*                                            display = nullptr;
    CSharedPointer<CCWlRegistry>                           registry;
    CSharedPointer<CCWlShm>                                shm;
    CSharedPointer<CCWpColorManagerV1>                     color;
    CSharedPointer<CCXxImageCaptureColorManagerV1>         captureColor;
    CSharedPointer<CCExtOutputImageCaptureSourceManagerV1> sources;
    CSharedPointer<CCExtImageCopyCaptureManagerV1>         capture;
    std::vector<CSharedPointer<SOutput>>                   outputs;
    uint32_t                                               width = 0, height = 0;
    bool                                                   tenBit = false;
    bool                                                   ready  = false;
    bool                                                   failed = false;
};

static bool bindGlobals(SState& state) {
    state.registry = makeShared<CCWlRegistry>(rc<wl_proxy*>(wl_display_get_registry(state.display)));
    state.registry->setGlobal([&state](CCWlRegistry* registry, uint32_t name, const char* interface, uint32_t version) {
        const auto bind = [&](const wl_interface* iface, uint32_t ver) { return rc<wl_proxy*>(wl_registry_bind(rc<wl_registry*>(registry->resource()), name, iface, ver)); };
        const std::string_view IFACE = interface;
        if (IFACE == "zwp_linux_dmabuf_v1" && version >= 3)
            state.linuxDma = makeShared<CCZwpLinuxDmabufV1>(bind(&zwp_linux_dmabuf_v1_interface, std::min(version, 4U)));
        else if (IFACE == "wl_shm")
            state.shm = makeShared<CCWlShm>(bind(&wl_shm_interface, 1));
        else if (IFACE == "wp_color_manager_v1" && version >= 2)
            state.color = makeShared<CCWpColorManagerV1>(bind(&wp_color_manager_v1_interface, std::min(version, 3U)));
        else if (IFACE == "xx_image_capture_color_manager_v1")
            state.captureColor = makeShared<CCXxImageCaptureColorManagerV1>(bind(&xx_image_capture_color_manager_v1_interface, 1));
        else if (IFACE == "ext_output_image_capture_source_manager_v1")
            state.sources = makeShared<CCExtOutputImageCaptureSourceManagerV1>(bind(&ext_output_image_capture_source_manager_v1_interface, 1));
        else if (IFACE == "ext_image_copy_capture_manager_v1")
            state.capture = makeShared<CCExtImageCopyCaptureManagerV1>(bind(&ext_image_copy_capture_manager_v1_interface, 1));
        else if (IFACE == "wl_output" && version >= 4) {
            const auto OUTPUT = state.outputs.emplace_back(makeShared<SOutput>());
            OUTPUT->resource  = makeShared<CCWlOutput>(bind(&wl_output_interface, 4));
            OUTPUT->resource->setName([output = OUTPUT.get()](CCWlOutput*, const char* value) { output->name = value; });
        }
    });
    state.registry->setGlobalRemove([](CCWlRegistry*, uint32_t) {});
    return wl_display_roundtrip(state.display) >= 0 && wl_display_roundtrip(state.display) >= 0 && state.shm && state.sources && state.capture &&
        (state.options.color == COLOR_UNTAGGED || (state.color && state.captureColor)) && (!state.options.dma || state.linuxDma);
}

static bool waitForFrame(SState& state) {
    const auto DEADLINE = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!state.ready && !state.failed) {
        while (wl_display_prepare_read(state.display) != 0) {
            if (wl_display_dispatch_pending(state.display) < 0)
                return false;
            if (state.ready || state.failed)
                return state.ready;
        }
        if (wl_display_flush(state.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(state.display);
            return false;
        }
        const auto MS = std::chrono::duration_cast<std::chrono::milliseconds>(DEADLINE - std::chrono::steady_clock::now()).count();
        if (MS <= 0) {
            wl_display_cancel_read(state.display);
            return false;
        }
        pollfd     fd     = {.fd = wl_display_get_fd(state.display), .events = POLLIN, .revents = 0};
        const auto RESULT = poll(&fd, 1, sc<int>(MS));
        if (RESULT <= 0) {
            wl_display_cancel_read(state.display);
            if (RESULT < 0 && errno == EINTR)
                continue;
            return false;
        }
        if (wl_display_read_events(state.display) < 0 || wl_display_dispatch_pending(state.display) < 0)
            return false;
    }
    return state.ready;
}

static bool capture(SState& state, std::string_view outputName, const std::string& path) {
    const auto OUTPUT = std::ranges::find_if(state.outputs, [outputName](const auto& output) { return output->name == outputName; });
    if (OUTPUT == state.outputs.end()) {
        std::println(stderr, "No output named {}", outputName);
        return false;
    }

    CSharedPointer<CCWpImageDescriptionV1> description;
    if (state.options.color != COLOR_UNTAGGED) {
        auto       params = makeShared<CCWpImageDescriptionCreatorParamsV1>(state.color->sendCreateParametricCreator());
        const bool hdr    = state.options.color == COLOR_PQ;
        params->sendSetTfNamed(hdr ? WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ : WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4);
        params->sendSetPrimariesNamed(hdr ? WP_COLOR_MANAGER_V1_PRIMARIES_BT2020 : WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
        params->sendSetLuminances(hdr ? 0 : 2000, hdr ? 10000 : 80, hdr ? 203 : 80);
        description           = makeShared<CCWpImageDescriptionV1>(params->sendCreate());
        bool descriptionReady = false;
        description->setReady([&](CCWpImageDescriptionV1*, uint32_t) { descriptionReady = true; });
        description->setReady2([&](CCWpImageDescriptionV1*, uint32_t, uint32_t) { descriptionReady = true; });
        description->setFailed([](CCWpImageDescriptionV1*, uint32_t cause, const char* message) { std::println(stderr, "Description failed: {} {}", cause, message); });
        if (wl_display_roundtrip(state.display) < 0 || !descriptionReady)
            return false;
        // No later callback may retain the stack's ready flag.
        description->setReady([](CCWpImageDescriptionV1*, uint32_t) {});
        description->setReady2([](CCWpImageDescriptionV1*, uint32_t, uint32_t) {});
    }

    auto source = makeShared<CCExtImageCaptureSourceV1>(state.sources->sendCreateSource((*OUTPUT)->resource->resource()));
    if (state.captureColor && state.color) {
        auto colors = makeShared<CCXxImageCaptureSourceColorsV1>(state.captureColor->sendGetCaptureSourceColors(source->resource()));
        colors->setPreferredChanged([](CCXxImageCaptureSourceColorsV1*, uint32_t hi, uint32_t lo) { std::println("Preferred capture identity: {}:{}", hi, lo); });
        auto reference      = makeShared<CCWpImageDescriptionReferenceV1>(colors->sendGetPreferred());
        auto preferred      = makeShared<CCWpImageDescriptionV1>(state.color->sendGetImageDescription(reference.get()));
        bool preferredReady = false;
        preferred->setReady([&](CCWpImageDescriptionV1*, uint32_t) { preferredReady = true; });
        preferred->setReady2([&](CCWpImageDescriptionV1*, uint32_t, uint32_t) { preferredReady = true; });
        if (wl_display_roundtrip(state.display) < 0 || !preferredReady)
            return false;
        auto info = makeShared<CCWpImageDescriptionInfoV1>(preferred->sendGetInformation());
        info->setTfNamed([](CCWpImageDescriptionInfoV1*, uint32_t tf) { std::println("Preferred transfer function enum: {}", tf); });
        info->setPrimariesNamed([](CCWpImageDescriptionInfoV1*, uint32_t primaries) { std::println("Preferred primaries enum: {}", primaries); });
        if (wl_display_roundtrip(state.display) < 0)
            return false;
    }
    auto session = makeShared<CCExtImageCopyCaptureSessionV1>(state.capture->sendCreateSession(
        source->resource(), state.options.cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : sc<extImageCopyCaptureManagerV1Options>(0)));
    session->setBufferSize([&](CCExtImageCopyCaptureSessionV1*, uint32_t width, uint32_t height) {
        state.width  = width;
        state.height = height;
    });
    session->setShmFormat([&](CCExtImageCopyCaptureSessionV1*, uint32_t format) { state.tenBit |= format == WL_SHM_FORMAT_XBGR2101010; });
    session->setDmabufDevice([&](CCExtImageCopyCaptureSessionV1*, wl_array* device) {
        if (device->size != sizeof(state.device)) {
            state.failed = true;
            return;
        }
        std::memcpy(&state.device, device->data, sizeof(state.device));
    });
    session->setDmabufFormat([&](CCExtImageCopyCaptureSessionV1*, uint32_t format, wl_array* modifiers) {
        if (format != DRM_FORMAT_XBGR2101010)
            return;
        if (modifiers->size % sizeof(uint64_t) != 0) {
            state.failed = true;
            return;
        }
        state.modifiers.resize(modifiers->size / sizeof(uint64_t));
        if (modifiers->size)
            std::memcpy(state.modifiers.data(), modifiers->data, modifiers->size);
    });
    session->setStopped([&](CCExtImageCopyCaptureSessionV1*) { state.failed = true; });
    if (wl_display_roundtrip(state.display) < 0 || state.failed || (!state.options.dma && !state.tenBit) || state.width == 0 || state.height == 0)
        return false;

    if (state.width > sc<uint32_t>(std::numeric_limits<int32_t>::max() / 4) || state.height > sc<uint32_t>(std::numeric_limits<int32_t>::max()))
        return false;
    const uint64_t SIZE = sc<uint64_t>(state.width) * state.height * 4;
    if (SIZE > sc<uint64_t>(std::numeric_limits<int32_t>::max()) || SIZE > 256 * 1024 * 1024)
        return false;
    Hyprutils::OS::CFileDescriptor fd;
    CMapping                       mapping;
    CCaptureGpuBuffer              gpu;
    CSharedPointer<CCWlShmPool>    pool;
    CSharedPointer<CCWlBuffer>     buffer;
    if (state.options.dma) {
        if (!gpu.allocate(state.device, state.width, state.height, state.modifiers))
            return false;
        buffer = makeShared<CCWlBuffer>(gpu.createWaylandBuffer(*state.linuxDma));
    } else {
        fd = Hyprutils::OS::CFileDescriptor{memfd_create("hyprland-hdr-capture", MFD_CLOEXEC)};
        if (!fd.isValid() || ftruncate(fd.get(), sc<off_t>(SIZE)) < 0 || !mapping.map(fd.get(), SIZE))
            return false;
        pool   = makeShared<CCWlShmPool>(state.shm->sendCreatePool(fd.get(), sc<int32_t>(SIZE)));
        buffer = makeShared<CCWlBuffer>(pool->sendCreateBuffer(0, state.width, state.height, state.width * 4, WL_SHM_FORMAT_XBGR2101010));
    }
    if (!buffer || !buffer->resource())
        return false;
    const auto width = state.width, height = state.height;
    const auto started = std::chrono::steady_clock::now();
    for (uint32_t index = 0; index < state.options.frames; ++index) {
        state.ready = false;
        auto frame  = makeShared<CCExtImageCopyCaptureFrameV1>(session->sendCreateFrame());
        frame->setReady([&](CCExtImageCopyCaptureFrameV1*) { state.ready = true; });
        frame->setTransform([&](CCExtImageCopyCaptureFrameV1*, uint32_t transform) { state.failed |= transform != WL_OUTPUT_TRANSFORM_NORMAL; });
        frame->setFailed([&](CCExtImageCopyCaptureFrameV1*, uint32_t reason) {
            std::println(stderr, "Capture failed: {}", reason);
            state.failed = true;
        });
        if (description)
            state.captureColor->sendSetFrameImageDescription(frame->resource(), description->resource());
        frame->sendAttachBuffer(buffer->resource());
        frame->sendDamageBuffer(0, 0, width, height);
        frame->sendCapture();
        if (!waitForFrame(state) || state.failed || state.width != width || state.height != height)
            return false;
        frame->sendDestroy();
    }
    const auto            seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::vector<uint32_t> dmaPixels;
    if (state.options.dma && !state.options.discardPixels && !gpu.readPixels(dmaPixels))
        return false;
    std::ofstream pixels;
    if (!state.options.discardPixels) {
        pixels.open(path, std::ios::binary);
        pixels.write(state.options.dma ? rc<const char*>(dmaPixels.data()) : mapping.data(), SIZE);
        pixels.flush();
    }
    std::ofstream metadata(path + ".json");
    const bool    hdr = state.options.color == COLOR_PQ;
    metadata << std::format("{{\"contract\":\"{}\",\"primaries\":\"{}\",\"transfer\":\"{}\",\"range\":\"full\",\"format\":\"xbgr2101010\","
                            "\"width\":{},\"height\":{},\"stride\":{},\"backend\":\"{}\",\"frames\":{},\"seconds\":{},\"fps\":{},\"modifier\":{},\"cursor\":{}}}\n",
                            state.options.color == COLOR_UNTAGGED ? "hyprland-legacy-srgb-copy" : "explicit-frame-image-description", hdr ? "bt2020" : "bt709",
                            hdr ? "st2084-pq" : "srgb", width, height, width * 4, state.options.dma ? "dmabuf" : "shm", state.options.frames, seconds,
                            state.options.frames / seconds, gpu.modifier(), state.options.cursor);
    metadata.flush();
    std::println("Captured {}x{} {} XBGR2101010 via {}: {} frames at {:.2f} fps to {}", width, height, hdr ? "BT.2020/PQ" : "sRGB", state.options.dma ? "DMA-BUF" : "SHM",
                 state.options.frames, state.options.frames / seconds, path);
    return (state.options.discardPixels || pixels.good()) && metadata.good();
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::println(stderr, "Usage: color-managed-capture OUTPUT FRAME.raw [--dmabuf] [--no-cursor] [--srgb|--untagged] [--frames N] [--discard-pixels]");
        return 1;
    }
    SOptions options;
    for (int i = 3; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--dmabuf")
            options.dma = true;
        else if (argument == "--no-cursor")
            options.cursor = false;
        else if (argument == "--srgb")
            options.color = COLOR_SRGB;
        else if (argument == "--untagged")
            options.color = COLOR_UNTAGGED;
        else if (argument == "--discard-pixels")
            options.discardPixels = true;
        else if (argument == "--frames" && i + 1 < argc) {
            const std::string_view value  = argv[++i];
            const auto             result = std::from_chars(value.data(), value.data() + value.size(), options.frames);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || options.frames == 0 || options.frames > 10000)
                return 1;
        } else
            return 1;
    }
    auto* display = wl_display_connect(nullptr);
    if (!display)
        return 1;
    bool success = false;
    {
        SState state;
        state.options = options;
        state.display = display;
        if (bindGlobals(state))
            success = capture(state, argv[1], argv[2]);
        else
            std::println(stderr, "Required globals missing: opt-in color capture and wp-color-management >=2 are required");
    }
    wl_display_disconnect(display);
    return success ? 0 : 1;
}
