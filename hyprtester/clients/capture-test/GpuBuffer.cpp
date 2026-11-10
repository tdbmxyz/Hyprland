#include "GpuBuffer.hpp"

#include <algorithm>
#include <array>
#include <fcntl.h>
#include <gbm.h>
#include <print>
#include <string_view>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <hyprutils/memory/Casts.hpp>
#include <hyprutils/os/FileDescriptor.hpp>
#include <linux-dmabuf-v1.hpp>

using namespace Hyprutils::Memory;
using Hyprutils::OS::CFileDescriptor;

struct CCaptureGpuBuffer::SImpl {
    CFileDescriptor                                            fd;
    std::unique_ptr<gbm_device, decltype(&gbm_device_destroy)> device{nullptr, gbm_device_destroy};
    std::unique_ptr<gbm_bo, decltype(&gbm_bo_destroy)>         bo{nullptr, gbm_bo_destroy};
    EGLDisplay                                                 display      = EGL_NO_DISPLAY;
    EGLContext                                                 context      = EGL_NO_CONTEXT;
    EGLImageKHR                                                image        = EGL_NO_IMAGE_KHR;
    PFNEGLDESTROYIMAGEKHRPROC                                  destroyImage = nullptr;
    GLuint                                                     input = 0, output = 0, framebuffer = 0, program = 0, vao = 0;
    uint32_t                                                   width = 0, height = 0;

    ~SImpl() {
        if (display == EGL_NO_DISPLAY)
            return;
        if (context != EGL_NO_CONTEXT) {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
            if (program)
                glDeleteProgram(program);
            if (vao)
                glDeleteVertexArrays(1, &vao);
            if (framebuffer)
                glDeleteFramebuffers(1, &framebuffer);
            if (input)
                glDeleteTextures(1, &input);
            if (output)
                glDeleteTextures(1, &output);
        }
        if (image != EGL_NO_IMAGE_KHR && destroyImage)
            destroyImage(display, image);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT)
            eglDestroyContext(display, context);
        eglTerminate(display);
    }
};

CCaptureGpuBuffer::CCaptureGpuBuffer() : m_impl(std::make_unique<SImpl>()) {
    ;
}
CCaptureGpuBuffer::~CCaptureGpuBuffer() = default;

static GLuint compile(GLenum kind, const char* text) {
    const auto shader = glCreateShader(kind);
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success)
        return shader;
    std::array<char, 4096> log = {};
    glGetShaderInfoLog(shader, log.size(), nullptr, log.data());
    std::println(stderr, "DMA-BUF readback shader failed: {}", log.data());
    glDeleteShader(shader);
    return 0;
}

bool CCaptureGpuBuffer::allocate(dev_t deviceID, uint32_t width, uint32_t height, const std::vector<uint64_t>& modifiers) {
    if (m_impl->bo || width == 0 || height == 0 || width > 8192 || height > 8192)
        return false;
    drmDevice* rawDevice = nullptr;
    if (drmGetDeviceFromDevId(deviceID, 0, &rawDevice) != 0)
        return false;
    const auto                                       freeDevice = [](drmDevice* device) { drmFreeDevice(&device); };
    std::unique_ptr<drmDevice, decltype(freeDevice)> drmDevice{rawDevice, freeDevice};
    if (!(drmDevice->available_nodes & (1 << DRM_NODE_RENDER)))
        return false;
    auto& state = *m_impl;
    state.fd    = CFileDescriptor{open(drmDevice->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC)};
    if (!state.fd.isValid())
        return false;
    state.device.reset(gbm_create_device(state.fd.get()));
    if (!state.device)
        return false;
    for (const auto modifier : modifiers) {
        if (modifier == DRM_FORMAT_MOD_INVALID)
            continue;
        state.bo.reset(gbm_bo_create_with_modifiers2(state.device.get(), width, height, DRM_FORMAT_XBGR2101010, &modifier, 1, GBM_BO_USE_RENDERING));
        if (state.bo)
            break;
    }
    if (!state.bo && std::ranges::contains(modifiers, DRM_FORMAT_MOD_INVALID))
        state.bo.reset(gbm_bo_create(state.device.get(), width, height, DRM_FORMAT_XBGR2101010, GBM_BO_USE_RENDERING));
    if (!state.bo) {
        std::println(stderr, "No advertised RGB10 DMA-BUF modifier could be allocated");
        return false;
    }
    const auto modifier = gbm_bo_get_modifier(state.bo.get());
    if (!std::ranges::contains(modifiers, modifier) && !std::ranges::contains(modifiers, DRM_FORMAT_MOD_INVALID))
        return false;
    state.width  = width;
    state.height = height;
    std::println(stderr, "Allocated RGB10 DMA-BUF {}x{}, modifier={:#x}, planes={}", width, height, modifier, gbm_bo_get_plane_count(state.bo.get()));
    return true;
}

wl_proxy* CCaptureGpuBuffer::createWaylandBuffer(CCZwpLinuxDmabufV1& manager) {
    if (!m_impl->bo)
        return nullptr;
    auto&                    state = *m_impl;
    CCZwpLinuxBufferParamsV1 params(manager.sendCreateParams());
    const auto               modifier = gbm_bo_get_modifier(state.bo.get());
    const auto               planes   = gbm_bo_get_plane_count(state.bo.get());
    for (int plane = 0; plane < planes; ++plane) {
        CFileDescriptor fd{gbm_bo_get_fd_for_plane(state.bo.get(), plane)};
        if (!fd.isValid())
            return nullptr;
        params.sendAdd(fd.get(), plane, gbm_bo_get_offset(state.bo.get(), plane), gbm_bo_get_stride_for_plane(state.bo.get(), plane), sc<uint32_t>(modifier >> 32),
                       sc<uint32_t>(modifier));
    }
    auto* buffer = params.sendCreateImmed(state.width, state.height, DRM_FORMAT_XBGR2101010, sc<zwpLinuxBufferParamsV1Flags>(0));
    params.sendDestroy();
    return buffer;
}

uint64_t CCaptureGpuBuffer::modifier() const {
    return m_impl->bo ? gbm_bo_get_modifier(m_impl->bo.get()) : DRM_FORMAT_MOD_INVALID;
}

bool CCaptureGpuBuffer::readPixels(std::vector<uint32_t>& pixels) {
    auto& state = *m_impl;
    if (!state.bo)
        return false;
    state.display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, state.device.get(), nullptr);
    if (state.display == EGL_NO_DISPLAY || !eglInitialize(state.display, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_ES_API))
        return false;
    constexpr std::array<EGLint, 7> CONFIG = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_NONE};
    EGLConfig                       config = nullptr;
    EGLint                          count  = 0;
    if (!eglChooseConfig(state.display, CONFIG.data(), &config, 1, &count) || count == 0)
        return false;
    constexpr std::array<EGLint, 3> CONTEXT = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    state.context                           = eglCreateContext(state.display, config, EGL_NO_CONTEXT, CONTEXT.data());
    if (state.context == EGL_NO_CONTEXT || !eglMakeCurrent(state.display, EGL_NO_SURFACE, EGL_NO_SURFACE, state.context))
        return false;
    const auto createImage = rc<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    state.destroyImage     = rc<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    const auto bindImage   = rc<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!createImage || !state.destroyImage || !bindImage)
        return false;
    const auto planes = gbm_bo_get_plane_count(state.bo.get());
    if (planes < 1 || planes > 4)
        return false;
    std::vector<EGLint>          attrs  = {EGL_WIDTH, sc<EGLint>(state.width), EGL_HEIGHT, sc<EGLint>(state.height), EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XBGR2101010};
    const std::array<EGLint, 4>  FD     = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
    const std::array<EGLint, 4>  OFFSET = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    const std::array<EGLint, 4>  PITCH  = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
    const std::array<EGLint, 4>  MOD_LO = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
                                           EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    const std::array<EGLint, 4>  MOD_HI = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
                                           EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
    std::vector<CFileDescriptor> fds;
    const auto                   modifier = gbm_bo_get_modifier(state.bo.get());
    for (int plane = 0; plane < planes; ++plane) {
        fds.emplace_back(gbm_bo_get_fd_for_plane(state.bo.get(), plane));
        if (!fds.back().isValid())
            return false;
        attrs.insert(attrs.end(),
                     {FD[plane], fds.back().get(), OFFSET[plane], sc<EGLint>(gbm_bo_get_offset(state.bo.get(), plane)), PITCH[plane],
                      sc<EGLint>(gbm_bo_get_stride_for_plane(state.bo.get(), plane))});
        if (modifier != DRM_FORMAT_MOD_INVALID)
            attrs.insert(attrs.end(), {MOD_LO[plane], sc<EGLint>(modifier & 0xFFFFFFFFU), MOD_HI[plane], sc<EGLint>(modifier >> 32)});
    }
    attrs.push_back(EGL_NONE);
    state.image = createImage(state.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
    if (state.image == EGL_NO_IMAGE_KHR) {
        std::println(stderr, "DMA-BUF EGL import failed: {:#x}", eglGetError());
        return false;
    }
    glGenTextures(1, &state.input);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, state.input);
    bindImage(GL_TEXTURE_EXTERNAL_OES, state.image);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Quantize directly into an integer target: no RGBA8 staging/readback.
    glGenTextures(1, &state.output);
    glBindTexture(GL_TEXTURE_2D, state.output);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16UI, state.width, state.height);
    glGenFramebuffers(1, &state.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, state.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, state.output, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return false;
    constexpr const char* VERTEX   = R"(#version 300 es
        out vec2 uv;
        void main() {
            vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
            uv = pos;
            gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
        }
    )";
    constexpr const char* FRAGMENT = R"(#version 300 es
        #extension GL_OES_EGL_image_external_essl3 : require
        precision highp float;
        precision highp int;
        uniform highp samplerExternalOES source;
        in vec2 uv;
        layout(location=0) out highp uvec4 result;
        void main() {
            result = uvec4(round(clamp(texture(source, uv), 0.0, 1.0) * 1023.0));
        }
    )";
    const auto            vertex   = compile(GL_VERTEX_SHADER, VERTEX);
    const auto            fragment = compile(GL_FRAGMENT_SHADER, FRAGMENT);
    if (!vertex || !fragment) {
        if (vertex)
            glDeleteShader(vertex);
        if (fragment)
            glDeleteShader(fragment);
        return false;
    }
    state.program = glCreateProgram();
    glAttachShader(state.program, vertex);
    glAttachShader(state.program, fragment);
    glLinkProgram(state.program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = 0;
    glGetProgramiv(state.program, GL_LINK_STATUS, &linked);
    if (!linked)
        return false;
    glUseProgram(state.program);
    glUniform1i(glGetUniformLocation(state.program, "source"), 0);
    glGenVertexArrays(1, &state.vao);
    glBindVertexArray(state.vao);
    glViewport(0, 0, state.width, state.height);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    std::vector<uint16_t> rgba(sc<size_t>(state.width) * state.height * 4);
    glReadPixels(0, 0, state.width, state.height, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, rgba.data());
    const auto error = glGetError();
    if (error != GL_NO_ERROR) {
        std::println(stderr, "DMA-BUF precision readback failed: {:#x}", error);
        return false;
    }
    pixels.resize(sc<size_t>(state.width) * state.height);
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = (3U << 30) | rgba[i * 4] | (sc<uint32_t>(rgba[i * 4 + 1]) << 10) | (sc<uint32_t>(rgba[i * 4 + 2]) << 20);
    return true;
}
