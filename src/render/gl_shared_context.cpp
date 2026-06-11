#include "render/gl_shared_context.h"

#include "core/log.h"

#include <EGL/eglext.h>
#include <format>
#include <stdexcept>
#include <string_view>
#include <vector>

#ifndef EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT
#define EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT 0x3138
#endif
#ifndef EGL_LOSE_CONTEXT_ON_RESET_EXT
#define EGL_LOSE_CONTEXT_ON_RESET_EXT 0x31BF
#endif
#ifndef EGL_GENERATE_RESET_ON_VIDEO_MEMORY_PURGE_NV
#define EGL_GENERATE_RESET_ON_VIDEO_MEMORY_PURGE_NV 0x334C
#endif

// GLES 3 renderable bit (EGL_KHR_create_context). Same value as EGL_OPENGL_ES3_BIT.
#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif
// EGL_EXT_pixel_format_float — lets a config request floating-point color buffers.
#ifndef EGL_COLOR_COMPONENT_TYPE_EXT
#define EGL_COLOR_COMPONENT_TYPE_EXT 0x3339
#endif
#ifndef EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT
#define EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT 0x333B
#endif

namespace {

  constexpr Logger kLog("gl");

  // Legacy 8-bit sRGB config (GLES 2). The unconditional fallback.
  constexpr EGLint kConfigAttributes[] = {
      EGL_SURFACE_TYPE,
      EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE,
      EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE,
      8,
      EGL_GREEN_SIZE,
      8,
      EGL_BLUE_SIZE,
      8,
      EGL_ALPHA_SIZE,
      8,
      EGL_NONE,
  };

  // fp16 (RGBA16F) GLES 3 config — the wide-colour HDR buffer (see chooseConfig).
  constexpr EGLint kFloatConfigAttributes[] = {
      EGL_SURFACE_TYPE,
      EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE,
      EGL_OPENGL_ES3_BIT_KHR,
      EGL_COLOR_COMPONENT_TYPE_EXT,
      EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT,
      EGL_RED_SIZE,
      16,
      EGL_GREEN_SIZE,
      16,
      EGL_BLUE_SIZE,
      16,
      EGL_ALPHA_SIZE,
      16,
      EGL_NONE,
  };

  bool hasExtension(const char* extensions, std::string_view name) {
    if (extensions == nullptr || name.empty()) {
      return false;
    }

    std::string_view list(extensions);
    std::size_t pos = 0;
    while (pos < list.size()) {
      while (pos < list.size() && list[pos] == ' ') {
        ++pos;
      }
      const std::size_t end = list.find(' ', pos);
      const std::string_view token = list.substr(pos, end == std::string_view::npos ? list.size() - pos : end - pos);
      if (token == name) {
        return true;
      }
      if (end == std::string_view::npos) {
        break;
      }
      pos = end + 1;
    }
    return false;
  }

  // Log the per-channel sizes eglChooseConfig actually returned (it matches "at
  // least" what we asked), so the log is the real answer to "are surfaces fp16?".
  void logConfigDepth(EGLDisplay display, EGLConfig config) {
    EGLint r = 0;
    EGLint g = 0;
    EGLint b = 0;
    EGLint a = 0;
    eglGetConfigAttrib(display, config, EGL_RED_SIZE, &r);
    eglGetConfigAttrib(display, config, EGL_GREEN_SIZE, &g);
    eglGetConfigAttrib(display, config, EGL_BLUE_SIZE, &b);
    eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE, &a);

    EGLint componentType = 0;
    const bool isFloat = eglGetConfigAttrib(display, config, EGL_COLOR_COMPONENT_TYPE_EXT, &componentType) == EGL_TRUE
                         && componentType == EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT;
    // Querying an unsupported attribute leaves an error latched; clear it.
    eglGetError();

    kLog.info("chosen EGLConfig: R{} G{} B{} A{} bits, component type {}", r, g, b, a, isFloat ? "float" : "fixed");
  }

} // namespace

GlSharedContext::~GlSharedContext() { cleanup(); }

void GlSharedContext::initialize(wl_display* display, bool createSharedContext) {
  if (display == nullptr) {
    throw std::runtime_error("GlSharedContext requires a valid Wayland display");
  }

  m_display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display));
  if (m_display == EGL_NO_DISPLAY) {
    throw std::runtime_error("eglGetDisplay failed");
  }

  EGLint major = 0;
  EGLint minor = 0;
  if (eglInitialize(m_display, &major, &minor) != EGL_TRUE) {
    throw std::runtime_error("eglInitialize failed");
  }

  if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
    throw std::runtime_error("eglBindAPI failed");
  }

  chooseConfig(eglQueryString(m_display, EGL_EXTENSIONS));

  buildContextAttributes();

  if (createSharedContext) {
    m_rootContext = createContext(EGL_NO_CONTEXT, "root");
    kLog.info("initialized EGL {}.{} with shared root context", major, minor);
  } else {
    kLog.info("initialized EGL {}.{} without shared context (isolated GPU contexts)", major, minor);
  }
}

void GlSharedContext::chooseConfig(const char* eglExtensions) {
  // Prefer an fp16 (RGBA16F) GLES 3 config, else fall back to 8-bit GLES 2.
  // Gated solely on EGL_EXT_pixel_format_float — the HDR colorspace is negotiated
  // over wp_color_manager_v1, not via an EGL colorspace extension.
  const bool hasPixelFormatFloat = hasExtension(eglExtensions, "EGL_EXT_pixel_format_float");
  kLog.info("EGL HDR-relevant extensions: pixel_format_float={}", hasPixelFormatFloat ? "yes" : "no");

  EGLint configCount = 0;
  if (hasPixelFormatFloat
      && eglChooseConfig(m_display, kFloatConfigAttributes, &m_config, 1, &configCount) == EGL_TRUE
      && configCount == 1) {
    m_colorPipeline = ColorPipeline::Float16;
    m_glesMajorVersion = 3;
    kLog.info("selected fp16 wide-colour config (GLES 3, RGBA16F)");
    logConfigDepth(m_display, m_config);
    return;
  }

  if (eglChooseConfig(m_display, kConfigAttributes, &m_config, 1, &configCount) != EGL_TRUE || configCount != 1) {
    throw std::runtime_error("eglChooseConfig failed");
  }
  m_colorPipeline = ColorPipeline::Srgb8;
  m_glesMajorVersion = 2;
  kLog.info("selected 8-bit RGBA config (GLES 2{})", hasPixelFormatFloat ? "; fp16 config unavailable" : "");
  logConfigDepth(m_display, m_config);
}

void GlSharedContext::buildContextAttributes() {
  const char* extensions = eglQueryString(m_display, EGL_EXTENSIONS);
  const bool hasRobustness = hasExtension(extensions, "EGL_EXT_create_context_robustness");
  const bool hasVideoMemoryPurge = hasExtension(extensions, "EGL_NV_robustness_video_memory_purge");

  m_contextAttributes.clear();
  m_contextAttributes.push_back(EGL_CONTEXT_CLIENT_VERSION);
  m_contextAttributes.push_back(m_glesMajorVersion);
  if (hasRobustness) {
    m_contextAttributes.push_back(EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT);
    m_contextAttributes.push_back(EGL_LOSE_CONTEXT_ON_RESET_EXT);
  }
  if (hasRobustness && hasVideoMemoryPurge) {
    m_contextAttributes.push_back(EGL_GENERATE_RESET_ON_VIDEO_MEMORY_PURGE_NV);
    m_contextAttributes.push_back(EGL_TRUE);
  }
  m_contextAttributes.push_back(EGL_NONE);

  m_contextAttributesRobust = hasRobustness;
  m_resetNotificationEnabled = hasRobustness;
  m_videoMemoryPurgeNotificationEnabled = hasRobustness && hasVideoMemoryPurge;

  if (m_videoMemoryPurgeNotificationEnabled) {
    kLog.info("requesting robust EGL contexts with NVIDIA video-memory purge reset notification");
  } else if (m_resetNotificationEnabled) {
    kLog.info("requesting robust EGL contexts with graphics reset notification");
  } else {
    kLog.info("EGL robustness context extension unavailable; using plain GLES contexts");
  }
}

EGLContext GlSharedContext::createContext(EGLContext shareContext, std::string_view label) {
  EGLContext context = createContextWithCurrentAttributes(shareContext);
  if (context != EGL_NO_CONTEXT) {
    return context;
  }

  const EGLint robustError = eglGetError();
  if (!m_contextAttributesRobust || shareContext != EGL_NO_CONTEXT) {
    throw std::runtime_error(
        std::format("eglCreateContext ({}) failed (EGL error 0x{:04x})", label, static_cast<unsigned>(robustError))
    );
  }

  kLog.warn(
      "robust eglCreateContext ({}) failed (EGL error 0x{:04x}); retrying with plain GLES context", label,
      static_cast<unsigned>(robustError)
  );
  usePlainContextAttributes();
  context = createContextWithCurrentAttributes(shareContext);
  if (context == EGL_NO_CONTEXT) {
    throw std::runtime_error(
        std::format("eglCreateContext ({}) failed (EGL error 0x{:04x})", label, static_cast<unsigned>(eglGetError()))
    );
  }
  return context;
}

EGLContext GlSharedContext::createContextWithCurrentAttributes(EGLContext shareContext) const {
  return eglCreateContext(m_display, m_config, shareContext, m_contextAttributes.data());
}

void GlSharedContext::usePlainContextAttributes() noexcept {
  m_contextAttributes.clear();
  m_contextAttributes.push_back(EGL_CONTEXT_CLIENT_VERSION);
  m_contextAttributes.push_back(m_glesMajorVersion);
  m_contextAttributes.push_back(EGL_NONE);
  m_contextAttributesRobust = false;
  m_resetNotificationEnabled = false;
  m_videoMemoryPurgeNotificationEnabled = false;
}

void GlSharedContext::makeCurrentSurfaceless() const {
  if (m_display == EGL_NO_DISPLAY || m_rootContext == EGL_NO_CONTEXT) {
    return;
  }
  if (eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_rootContext) != EGL_TRUE) {
    throw std::runtime_error(
        std::format(
            "eglMakeCurrent (root, surfaceless) failed (EGL error 0x{:04x})", static_cast<unsigned>(eglGetError())
        )
    );
  }
}

void GlSharedContext::cleanup() {
  if (m_display == EGL_NO_DISPLAY) {
    return;
  }

  eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

  if (m_rootContext != EGL_NO_CONTEXT) {
    eglDestroyContext(m_display, m_rootContext);
    m_rootContext = EGL_NO_CONTEXT;
  }

  eglTerminate(m_display);
  m_display = EGL_NO_DISPLAY;
  m_config = nullptr;
  m_colorPipeline = ColorPipeline::Srgb8;
  m_glesMajorVersion = 2;
  m_contextAttributes.clear();
  m_contextAttributesRobust = false;
  m_resetNotificationEnabled = false;
  m_videoMemoryPurgeNotificationEnabled = false;
}
