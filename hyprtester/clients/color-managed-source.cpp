// Native RGB10 Wayland fixture. Run only on an isolated HDR_CAPTURE_LAB socket:
// bright reference patches must never be shown accidentally on a physical panel.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <print>
#include <span>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland.hpp>
#include <xdg-shell.hpp>
#include <color-management-v1.hpp>
#include <wlr-virtual-pointer-unstable-v1.hpp>
#include <ext-session-lock-v1.hpp>
#include <hyprutils/memory/Casts.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/os/FileDescriptor.hpp>

using namespace Hyprutils::Memory;

struct SBuffer {
    CSharedPointer<CCWlShmPool> pool;
    CSharedPointer<CCWlBuffer>  buffer;
};

struct SState {
    wl_display*                                   display = nullptr;
    CSharedPointer<CCWlRegistry>                  registry;
    CSharedPointer<CCWlCompositor>                compositor;
    CSharedPointer<CCWlSubcompositor>             subcompositor;
    CSharedPointer<CCWlShm>                       shm;
    CSharedPointer<CCXdgWmBase>                   shell;
    CSharedPointer<CCWpColorManagerV1>            color;
    CSharedPointer<CCZwlrVirtualPointerManagerV1> virtualPointerManager;
    CSharedPointer<CCZwlrVirtualPointerV1>        virtualPointer;
    CSharedPointer<CCExtSessionLockManagerV1>     lockManager;
    CSharedPointer<CCWlSeat>                      seat;
    CSharedPointer<CCWlPointer>                   pointer;
    CSharedPointer<CCWlSurface>                   surface, sdrSurface, cursorSurface;
    CSharedPointer<CCWlSubsurface>                sdrSubsurface;
    CSharedPointer<CCXdgSurface>                  xdgSurface;
    CSharedPointer<CCXdgToplevel>                 toplevel;
    CSharedPointer<CCWpColorManagementSurfaceV1>  colorSurface;
    CSharedPointer<CCWpImageDescriptionV1>        description;
    SBuffer                                       hdr, sdr, cursor;
    uint32_t                                      width = 1024, height = 512;
    bool                                          hasRGB10         = false;
    bool                                          descriptionReady = false;
    bool                                          failed           = false;
    bool                                          cursorEnabled    = false;
};

static uint32_t pq10(double nits) {
    const auto power = std::pow(nits / 10000, 2610.0 / 16384);
    return sc<uint32_t>(std::lround(std::pow((3424.0 / 4096 + 2413.0 / 128 * power) / (1 + 2392.0 / 128 * power), 2523.0 / 32) * 1023));
}

static uint32_t pattern(uint32_t x, uint32_t y) {
    const auto     ramp  = x % 1024;
    const auto     band  = y % 512;
    const uint32_t alpha = 3U << 30;
    if (band < 64)
        return alpha | ramp | (ramp << 10) | (ramp << 20);
    if (band < 128)
        return alpha | ramp;
    if (band < 192)
        return alpha | (ramp << 10);
    if (band < 256)
        return alpha | (ramp << 20);
    if (band < 320) {
        constexpr std::array<double, 8> NITS = {0, 80, 160, 203, 400, 1000, 4000, 10000};
        const auto                      code = pq10(NITS[ramp / 128]);
        return alpha | code | (code << 10) | (code << 20);
    }
    return alpha;
}

static bool makeBuffer(SState& state, SBuffer& target, uint32_t width, uint32_t height, bool hdr, bool cursor = false) {
    if (width == 0 || height == 0 || width > 8192 || height > 8192)
        return false;
    const auto                     size = sc<size_t>(width) * height * 4;
    Hyprutils::OS::CFileDescriptor fd{memfd_create("hdr-native-source", MFD_CLOEXEC)};
    if (!fd.isValid() || ftruncate(fd.get(), size) < 0)
        return false;
    auto* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
    if (memory == MAP_FAILED)
        return false;
    std::span<uint32_t> pixels{sc<uint32_t*>(memory), size / 4};
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            if (hdr)
                pixels[y * width + x] = pattern(x, y);
            else if (cursor)
                pixels[y * width + x] = 0xFFFFFFFFU;
            else
                pixels[y * width + x] = x < width / 2 ? 0xFFFFFFFFU : 0xFF808080U;
        }
    }
    munmap(memory, size);
    if (target.buffer)
        target.buffer->sendDestroy();
    if (target.pool)
        target.pool->sendDestroy();
    target.pool   = makeShared<CCWlShmPool>(state.shm->sendCreatePool(fd.get(), sc<int32_t>(size)));
    target.buffer = makeShared<CCWlBuffer>(target.pool->sendCreateBuffer(0, width, height, width * 4, hdr ? WL_SHM_FORMAT_XBGR2101010 : WL_SHM_FORMAT_ARGB8888));
    return target.buffer->resource();
}

static bool bindGlobals(SState& state) {
    state.registry = makeShared<CCWlRegistry>(rc<wl_proxy*>(wl_display_get_registry(state.display)));
    state.registry->setGlobal([&](CCWlRegistry* registry, uint32_t id, const char* interface, uint32_t version) {
        const auto bind = [&](const wl_interface* iface, uint32_t ver) {
            return rc<wl_proxy*>(wl_registry_bind(rc<wl_registry*>(registry->resource()), id, iface, std::min(version, ver)));
        };
        const std::string_view name = interface;
        if (name == "wl_compositor")
            state.compositor = makeShared<CCWlCompositor>(bind(&wl_compositor_interface, 6));
        else if (name == "wl_subcompositor")
            state.subcompositor = makeShared<CCWlSubcompositor>(bind(&wl_subcompositor_interface, 1));
        else if (name == "wl_shm") {
            state.shm = makeShared<CCWlShm>(bind(&wl_shm_interface, 1));
            state.shm->setFormat([&](CCWlShm*, uint32_t format) { state.hasRGB10 |= format == WL_SHM_FORMAT_XBGR2101010; });
        } else if (name == "xdg_wm_base") {
            state.shell = makeShared<CCXdgWmBase>(bind(&xdg_wm_base_interface, 1));
            state.shell->setPing([](CCXdgWmBase* shell, uint32_t serial) { shell->sendPong(serial); });
        } else if (name == "wp_color_manager_v1")
            state.color = makeShared<CCWpColorManagerV1>(bind(&wp_color_manager_v1_interface, 3));
        else if (name == "zwlr_virtual_pointer_manager_v1")
            state.virtualPointerManager = makeShared<CCZwlrVirtualPointerManagerV1>(bind(&zwlr_virtual_pointer_manager_v1_interface, 1));
        else if (name == "ext_session_lock_manager_v1")
            state.lockManager = makeShared<CCExtSessionLockManagerV1>(bind(&ext_session_lock_manager_v1_interface, 1));
        else if (name == "wl_seat") {
            state.seat = makeShared<CCWlSeat>(bind(&wl_seat_interface, 1));
            state.seat->setCapabilities([&](CCWlSeat* seat, uint32_t capabilities) {
                if (state.pointer || !(capabilities & WL_SEAT_CAPABILITY_POINTER))
                    return;
                state.pointer = makeShared<CCWlPointer>(seat->sendGetPointer());
                state.pointer->setEnter([&](CCWlPointer* pointer, uint32_t serial, wl_proxy*, int32_t, int32_t) {
                    if (state.cursorEnabled && state.cursorSurface)
                        pointer->sendSetCursor(serial, state.cursorSurface.get(), 0, 0);
                });
            });
        }
    });
    state.registry->setGlobalRemove([](CCWlRegistry*, uint32_t) {});
    return wl_display_roundtrip(state.display) >= 0 && wl_display_roundtrip(state.display) >= 0 && state.compositor && state.subcompositor && state.shm && state.shell &&
        state.color && state.hasRGB10;
}

static bool setup(SState& state) {
    auto params = makeShared<CCWpImageDescriptionCreatorParamsV1>(state.color->sendCreateParametricCreator());
    params->sendSetTfNamed(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
    params->sendSetPrimariesNamed(WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    params->sendSetLuminances(0, 10000, 203);
    state.description = makeShared<CCWpImageDescriptionV1>(params->sendCreate());
    state.description->setReady([&](CCWpImageDescriptionV1*, uint32_t) { state.descriptionReady = true; });
    state.description->setReady2([&](CCWpImageDescriptionV1*, uint32_t, uint32_t) { state.descriptionReady = true; });
    state.description->setFailed([&](CCWpImageDescriptionV1*, uint32_t, const char* message) {
        std::println(stderr, "Image description failed: {}", message);
        state.failed = true;
    });
    if (wl_display_roundtrip(state.display) < 0 || !state.descriptionReady)
        return false;
    state.surface      = makeShared<CCWlSurface>(state.compositor->sendCreateSurface());
    state.colorSurface = makeShared<CCWpColorManagementSurfaceV1>(state.color->sendGetSurface(state.surface->resource()));
    state.colorSurface->sendSetImageDescription(state.description.get(), WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    state.xdgSurface = makeShared<CCXdgSurface>(state.shell->sendGetXdgSurface(state.surface->resource()));
    state.toplevel   = makeShared<CCXdgToplevel>(state.xdgSurface->sendGetToplevel());
    state.toplevel->sendSetAppId("hdr-capture-source");
    state.toplevel->sendSetTitle("Native RGB10 capture reference (isolated lab)");
    state.toplevel->sendSetFullscreen(nullptr);
    state.toplevel->setClose([&](CCXdgToplevel*) { state.failed = true; });
    state.toplevel->setConfigure([&](CCXdgToplevel*, int32_t width, int32_t height, wl_array*) {
        if (width > 0 && height > 0) {
            state.width  = width;
            state.height = height;
        }
    });
    state.sdrSurface    = makeShared<CCWlSurface>(state.compositor->sendCreateSurface());
    state.sdrSubsurface = makeShared<CCWlSubsurface>(state.subcompositor->sendGetSubsurface(state.sdrSurface.get(), state.surface.get()));
    state.sdrSubsurface->sendSetPosition(0, 320);
    if (!makeBuffer(state, state.sdr, 512, 64, false))
        return false;
    state.sdrSurface->sendAttach(state.sdr.buffer.get(), 0, 0);
    state.sdrSurface->sendCommit();
    if (state.cursorEnabled) {
        if (!state.virtualPointerManager || !state.seat)
            return false;
        state.cursorSurface = makeShared<CCWlSurface>(state.compositor->sendCreateSurface());
        if (!makeBuffer(state, state.cursor, 16, 16, false, true))
            return false;
        state.cursorSurface->sendAttach(state.cursor.buffer.get(), 0, 0);
        state.cursorSurface->sendCommit();
        state.virtualPointer = makeShared<CCZwlrVirtualPointerV1>(state.virtualPointerManager->sendCreateVirtualPointer(state.seat->resource()));
    }
    state.xdgSurface->setConfigure([&](CCXdgSurface* surface, uint32_t serial) {
        surface->sendAckConfigure(serial);
        if (!makeBuffer(state, state.hdr, state.width, state.height, true)) {
            state.failed = true;
            return;
        }
        state.surface->sendAttach(state.hdr.buffer.get(), 0, 0);
        state.surface->sendDamageBuffer(0, 0, state.width, state.height);
        state.surface->sendCommit();
        if (state.virtualPointer) {
            state.virtualPointer->sendMotionAbsolute(0, 900, 450, state.width, state.height);
            state.virtualPointer->sendFrame();
        }
        std::println(stderr, "Ready: {}x{} XBGR2101010 BT.2020/PQ; SDR child 512x64 at 0,320; cursor={}", state.width, state.height, state.cursorEnabled);
    });
    state.surface->sendCommit();
    return true;
}

int main(int argc, char** argv) {
    const auto* lab     = std::getenv("HDR_CAPTURE_LAB");
    const auto* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!lab || std::string_view(lab) != "1" || !runtime || !std::string_view(runtime).starts_with("/tmp/hh.")) {
        std::println(stderr, "Refusing bright test content/input injection without HDR_CAPTURE_LAB=1 (use an isolated compositor)");
        return 1;
    }
    auto* display = wl_display_connect(nullptr);
    if (!display)
        return 1;
    bool success = false;
    {
        SState state;
        state.display       = display;
        state.cursorEnabled = argc == 2 && std::string_view(argv[1]) == "--cursor";
        const bool lockMode = argc == 2 && std::string_view(argv[1]) == "--lock";
        if (argc > 2 || (argc == 2 && !state.cursorEnabled && !lockMode)) {
            std::println(stderr, "Usage: color-managed-source [--cursor|--lock]");
            wl_display_disconnect(display);
            return 1;
        }
        const bool bound = bindGlobals(state);
        if (bound && lockMode && state.lockManager) {
            auto lock = makeShared<CCExtSessionLockV1>(state.lockManager->sendLock());
            lock->setLocked([](CCExtSessionLockV1*) { std::println(stderr, "Isolated session locked"); });
            lock->setFinished([&](CCExtSessionLockV1*) { state.failed = true; });
            if (wl_display_roundtrip(display) >= 0)
                std::println(stderr, "Isolated lock request processed");
            while (!state.failed && wl_display_dispatch(display) >= 0) {
                ;
            }
        } else if (bound && !lockMode && setup(state)) {
            while (!state.failed && wl_display_dispatch(display) >= 0) {
                ;
            }
            success = !state.failed;
        }
    }
    wl_display_disconnect(display);
    return success ? 0 : 1;
}
