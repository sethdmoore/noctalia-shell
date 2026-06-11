#pragma once

#include <EGL/egl.h>
#include <string_view>
#include <vector>

struct wl_display;

// Color buffer format the shared EGL window config was negotiated for.
//   Srgb8   — legacy 8-bit RGBA (GLES 2); the unconditional fallback.
//   Float16 — fp16 (RGBA16F) GLES 3: 16-bit per channel incl. alpha, with >1.0
//             headroom. Its colorspace is negotiated via wp_color_manager_v1
//             (Wayland), not EGL. No RGB10_A2 tier — its 2-bit alpha bands panels.
enum class ColorPipeline {
  Srgb8,
  Float16,
};

// Owns the EGLDisplay, the chosen EGLConfig, and a root surfaceless EGLContext
// that is the share parent for every other EGLContext in the shell
// (RenderContext, WallpaperRenderer instances, ...). By routing all contexts
// through this share group, GL textures uploaded in any of them are usable
// from every other context in the group — which lets LockSurface reuse
// wallpaper textures resident in VRAM without re-decoding.
class GlSharedContext {
public:
  GlSharedContext() = default;
  ~GlSharedContext();

  GlSharedContext(const GlSharedContext&) = delete;
  GlSharedContext& operator=(const GlSharedContext&) = delete;

  void initialize(wl_display* display, bool createSharedContext = true);
  void cleanup();

  [[nodiscard]] EGLDisplay display() const noexcept { return m_display; }
  [[nodiscard]] EGLConfig config() const noexcept { return m_config; }
  [[nodiscard]] EGLContext rootContext() const noexcept { return m_rootContext; }
  [[nodiscard]] bool hasSharedContext() const noexcept { return m_rootContext != EGL_NO_CONTEXT; }

  // Negotiated color buffer format; floatColorBuffer() gates HDR image-description requests.
  [[nodiscard]] ColorPipeline colorPipeline() const noexcept { return m_colorPipeline; }
  [[nodiscard]] bool floatColorBuffer() const noexcept { return m_colorPipeline == ColorPipeline::Float16; }
  [[nodiscard]] bool resetNotificationEnabled() const noexcept { return m_resetNotificationEnabled; }
  [[nodiscard]] bool videoMemoryPurgeNotificationEnabled() const noexcept {
    return m_videoMemoryPurgeNotificationEnabled;
  }

  [[nodiscard]] EGLContext createContext(EGLContext shareContext, std::string_view label);

  // Bind the root context surfacelessly. No-op when shared context is disabled.
  void makeCurrentSurfaceless() const;

private:
  void chooseConfig(const char* eglExtensions);
  void buildContextAttributes();
  [[nodiscard]] EGLContext createContextWithCurrentAttributes(EGLContext shareContext) const;
  void usePlainContextAttributes() noexcept;

  EGLDisplay m_display = EGL_NO_DISPLAY;
  EGLConfig m_config = nullptr;
  EGLContext m_rootContext = EGL_NO_CONTEXT;
  ColorPipeline m_colorPipeline = ColorPipeline::Srgb8;
  EGLint m_glesMajorVersion = 2;
  std::vector<EGLint> m_contextAttributes;
  bool m_contextAttributesRobust = false;
  bool m_resetNotificationEnabled = false;
  bool m_videoMemoryPurgeNotificationEnabled = false;
};
