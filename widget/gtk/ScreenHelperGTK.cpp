/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ScreenHelperGTK.h"

#ifdef MOZ_X11
#  include <gdk/gdkx.h>
#  include <X11/Xlib.h>
#  include "X11UndefineNone.h"
#  include <X11/extensions/Xrandr.h>
#  include <X11/X.h>
#endif /* MOZ_X11 */
#ifdef MOZ_WAYLAND
#  include <gdk/gdkwayland.h>
#endif /* MOZ_WAYLAND */
#include <dlfcn.h>
#include <gtk/gtk.h>

#include "gfxPlatformGtk.h"
#include "mozilla/dom/DOMTypes.h"
#include "mozilla/Logging.h"
#include "mozilla/WidgetUtilsGtk.h"
#include "nsGtkUtils.h"
#include "nsTArray.h"
#include "nsWindow.h"
#include <iostream>
#include <string>
#include <sys/wait.h>

struct wl_registry;

#ifdef MOZ_WAYLAND
#  include "nsWaylandDisplay.h"
#endif

namespace mozilla::widget {

#ifdef MOZ_LOGGING
static LazyLogModule sScreenLog("WidgetScreen");
#  define LOG_SCREEN(...) MOZ_LOG(sScreenLog, LogLevel::Debug, (__VA_ARGS__))
#else
#  define LOG_SCREEN(...)
#endif /* MOZ_LOGGING */

using GdkMonitor = struct _GdkMonitor;

class ScreenGetterGtk final {
 public:
  ScreenGetterGtk() = default;
  ~ScreenGetterGtk();
  void Init();

#ifdef MOZ_X11
  Atom NetWorkareaAtom() { return mNetWorkareaAtom; }
#endif

  // For internal use from signal callback functions
  void RefreshScreens();

 private:
  GdkWindow* mRootWindow = nullptr;
#ifdef MOZ_X11
  Atom mNetWorkareaAtom = 0;
#endif
};

nsWindow* ScreenHelperGTK::mTopWindows;
static GdkMonitor* GdkDisplayGetMonitor(GdkDisplay* aDisplay, int aMonitorNum) {
  static auto s_gdk_display_get_monitor = (GdkMonitor * (*)(GdkDisplay*, int))
      dlsym(RTLD_DEFAULT, "gdk_display_get_monitor");
  if (!s_gdk_display_get_monitor) {
    return nullptr;
  }
  return s_gdk_display_get_monitor(aDisplay, aMonitorNum);
}

RefPtr<Screen> ScreenHelperGTK::GetScreenForWindow(nsWindow* aWindow) {
  LOG_SCREEN("GetScreenForWindow() [%p]", aWindow);

  static auto s_gdk_display_get_monitor_at_window =
      (GdkMonitor * (*)(GdkDisplay*, GdkWindow*))
          dlsym(RTLD_DEFAULT, "gdk_display_get_monitor_at_window");

  if (!s_gdk_display_get_monitor_at_window) {
    LOG_SCREEN("  failed, missing Gtk helpers");
    return nullptr;
  }

  GdkWindow* gdkWindow = gtk_widget_get_window(aWindow->GetGtkWidget());
  if (!gdkWindow) {
    LOG_SCREEN("  failed, can't get GdkWindow");
    return nullptr;
  }

  GdkDisplay* display = gdk_display_get_default();
  GdkMonitor* monitor = s_gdk_display_get_monitor_at_window(display, gdkWindow);
  if (!monitor) {
    LOG_SCREEN("  failed, can't get monitor for GdkWindow");
    return nullptr;
  }

  int index = -1;
  while (GdkMonitor* m = GdkDisplayGetMonitor(display, ++index)) {
    if (m == monitor) {
      return ScreenManager::GetSingleton().CurrentScreenList().SafeElementAt(
          index);
    }
  }

  LOG_SCREEN("  Couldn't find monitor %p", monitor);
  return nullptr;
}

static UniquePtr<ScreenGetterGtk> gScreenGetter;

static void monitors_changed(GdkScreen* aScreen, gpointer aClosure) {
  LOG_SCREEN("Received monitors-changed event");
  auto* self = static_cast<ScreenGetterGtk*>(aClosure);
  self->RefreshScreens();
}

static void screen_resolution_changed(GdkScreen* aScreen, GParamSpec* aPspec,
                                      ScreenGetterGtk* self) {
  self->RefreshScreens();
}

static GdkFilterReturn root_window_event_filter(GdkXEvent* aGdkXEvent,
                                                GdkEvent* aGdkEvent,
                                                gpointer aClosure) {
#ifdef MOZ_X11
  ScreenGetterGtk* self = static_cast<ScreenGetterGtk*>(aClosure);
  XEvent* xevent = static_cast<XEvent*>(aGdkXEvent);

  switch (xevent->type) {
    case PropertyNotify: {
      XPropertyEvent* propertyEvent = &xevent->xproperty;
      if (propertyEvent->atom == self->NetWorkareaAtom()) {
        LOG_SCREEN("Work area size changed");
        self->RefreshScreens();
      }
    } break;
    default:
      break;
  }
#endif

  return GDK_FILTER_CONTINUE;
}

static bool exec_xrandr(const std::vector<const char*>& args) {
  pid_t pid = fork();
  if (pid == 0) {
    // 子进程
    std::vector<char*> argv;
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg));
    }
    argv.push_back(nullptr);

    execvp("xrandr", argv.data());
    _exit(1);  // 如果 exec 失败
  } else if (pid > 0) {
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  } else {
    std::cerr << "Fork failed." << std::endl;
    return false;
  }
}
static std::string build_transform(double scale_x, int offset_x, double scale_y,
                                   int offset_y) {
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "%.10g,0,%d,0,%.10g,%d,0,0,1", scale_x,
           offset_x, scale_y, offset_y);
  return std::string(buffer);
}
static int32_t dis_num = 0;

static void activate_new_displays() {
  Display* display = XOpenDisplay(NULL);
  if (display == NULL) {
    fprintf(stderr, "Cannot open display\n");
    exit(1);
  }
  int event_base, error_base;
  if (!XRRQueryExtension(display, &event_base, &error_base)) {
    fprintf(stderr, "RandR extension not available\n");
    return;
  }

  Window root = DefaultRootWindow(display);
  XRRScreenResources* resources = XRRGetScreenResourcesCurrent(display, root);

  int current_num = 0;
  for (int i = 0; i < resources->noutput; ++i) {
    XRROutputInfo* output_info =
        XRRGetOutputInfo(display, resources, resources->outputs[i]);
    if (output_info->connection == RR_Connected) {
      current_num++;
    }
    XRRFreeOutputInfo(output_info);
  }
  if (current_num > dis_num) {
    XRROutputInfo* primary_output = NULL;
    XRRCrtcInfo* primary_crtc = NULL;
    int main_index = 0;

    for (int i = 0; i < resources->noutput; ++i) {
      XRROutputInfo* output_info =
          XRRGetOutputInfo(display, resources, resources->outputs[i]);

      if (output_info->connection == RR_Connected && output_info->crtc != 0) {
        primary_output = output_info;
        primary_crtc = XRRGetCrtcInfo(display, resources, output_info->crtc);
        main_index = i;
        printf("Found primary output: %s using CRTC: %d\n", output_info->name,
               output_info->crtc);
        break;
      }

      XRRFreeOutputInfo(output_info);
    }

    if (!primary_output || !primary_crtc) {
      fprintf(stderr, "Failed to find active primary output\n");
      return;
    }
    XRRModeInfo* primary_mode = NULL;
    for (int j = 0; j < resources->nmode; ++j) {
      if (resources->modes[j].id == primary_crtc->mode) {
        primary_mode = &resources->modes[j];
        break;
      }
    }

    if (!primary_mode) {
      fprintf(stderr, "Failed to get primary mode info\n");
      XRRFreeOutputInfo(primary_output);
      XRRFreeScreenResources(resources);
      XCloseDisplay(display);
      return;
    }
    int primary_width = primary_mode->width;
    int primary_height = primary_mode->height;
    printf("Primary display resolution: %dx%d\n", primary_width,
           primary_height);
    double primary_wh = (double)primary_width / primary_height;

    for (int i = 0; i < resources->noutput; ++i) {
      XRROutputInfo* output_info =
          XRRGetOutputInfo(display, resources, resources->outputs[i]);

      if (output_info->connection == RR_Connected && i != main_index) {
        printf("Found external display to mirror: %s\n", output_info->name);
        if (output_info->nmode > 0) {
          XRRModeInfo* external_mode = NULL;
          for (int j = 0; j < resources->nmode; ++j) {
            if (resources->modes[j].id == output_info->modes[0]) {
              external_mode = &resources->modes[j];
              break;
            }
          }

          if (!external_mode) {
            fprintf(stderr, "Failed to get external mode info\n");
            XRRFreeOutputInfo(primary_output);
            XRRFreeScreenResources(resources);
            XCloseDisplay(display);
            return;
          }

          int external_width = external_mode->width;
          int external_height = external_mode->height;
          printf("External display max resolution: %dx%d\n", external_width,
                 external_height);
          std::string p_n = primary_output->name;
          std::string o_n = output_info->name;

          double scale = (double)primary_width / external_width;
          if (scale < (double)primary_height / external_height) {
            scale = (double)primary_height / external_height;
          }
          // 居中偏移量
          double offset_x =
              (external_width - primary_width / scale) / 2.0 / external_width;
          double offset_y = (external_height - primary_height / scale) / 2.0 /
                            external_height;
          exec_xrandr({"xrandr", "--output", o_n.c_str(), "--off"});

          exec_xrandr({"xrandr", "--output", o_n.c_str(), "--auto"});
          std::string transform =
              build_transform(scale, -offset_x * external_width, scale,
                              -offset_y * external_height);
          bool exec_succ =
              exec_xrandr({"xrandr", "--output", o_n.c_str(), "--mode",
                           (std::to_string(external_width) + "x" +
                            std::to_string(external_height))
                               .c_str(),
                           "--pos", "0x0"});

          if (!exec_succ) {
            XRRFreeOutputInfo(primary_output);
            XRRFreeScreenResources(resources);
            XCloseDisplay(display);
            return;
          }
          exec_xrandr({"xrandr", "--output", o_n.c_str(), "--transform",
                       transform.c_str()});

        } else {
          fprintf(stderr, "External display %s has no valid modes\n",
                  output_info->name);
        }
      }
      XRRFreeOutputInfo(output_info);
    }
    XRRFreeOutputInfo(primary_output);
    dis_num = current_num;
  } else if (current_num < dis_num) {
    for (int i = 0; i < resources->noutput; ++i) {
      RROutput output_id = resources->outputs[i];
      XRROutputInfo* output_info =
          XRRGetOutputInfo(display, resources, output_id);
      if (!output_info) continue;
      std::string name = output_info->name;
      if (output_info->connection == RR_Connected) {
        XRRCrtcInfo* crtc_info =
            XRRGetCrtcInfo(display, resources, output_info->crtc);
        XRRModeInfo current_mode_info;
        for (int i = 0; i < resources->nmode; ++i) {
          if (resources->modes[i].id == crtc_info->mode) {
            current_mode_info = resources->modes[i];
            if (ScreenHelperGTK::mTopWindows) {
              ScreenHelperGTK::mTopWindows->Resize(
                  0, 0, resources->modes[i].width, resources->modes[i].height,
                  true);
            }
            break;
          }
        }
        XRRFreeOutputInfo(output_info);
        break;
      }
      XRRFreeOutputInfo(output_info);
    }
    dis_num = current_num;
  }

  XRRFreeScreenResources(resources);
  XCloseDisplay(display);
}

static gboolean check_monitors(gpointer user_data) {
  activate_new_displays();
  return G_SOURCE_CONTINUE;  // Continue checking periodically
}

void ScreenGetterGtk::Init() {
  LOG_SCREEN("ScreenGetterGtk created");
  dis_num = ScreenHelperGTK::GetMonitorCount();
  GdkScreen* defaultScreen = gdk_screen_get_default();
  if (!defaultScreen) {
    // Sometimes we don't initial X (e.g., xpcshell)
    MOZ_LOG(sScreenLog, LogLevel::Debug,
            ("defaultScreen is nullptr, running headless"));
    return;
  }
  mRootWindow = gdk_get_default_root_window();
  MOZ_ASSERT(mRootWindow);

  g_object_ref(mRootWindow);

  // GDK_PROPERTY_CHANGE_MASK ==> PropertyChangeMask, for PropertyNotify
  gdk_window_set_events(mRootWindow,
                        GdkEventMask(gdk_window_get_events(mRootWindow) |
                                     GDK_PROPERTY_CHANGE_MASK));

  g_timeout_add_seconds(0.2, check_monitors, NULL);
  g_signal_connect(defaultScreen, "monitors-changed",
                   G_CALLBACK(monitors_changed), this);
  // Use _after to ensure this callback is run after gfxPlatformGtk.cpp's
  // handler.
  g_signal_connect_after(defaultScreen, "notify::resolution",
                         G_CALLBACK(screen_resolution_changed), this);
#ifdef MOZ_X11
  gdk_window_add_filter(mRootWindow, root_window_event_filter, this);
  if (GdkIsX11Display()) {
    mNetWorkareaAtom = XInternAtom(GDK_WINDOW_XDISPLAY(mRootWindow),
                                   "_NET_WORKAREA", X11False);
  }
#endif
  RefreshScreens();
}

ScreenGetterGtk::~ScreenGetterGtk() {
  if (mRootWindow) {
    g_signal_handlers_disconnect_by_data(gdk_screen_get_default(), this);

    gdk_window_remove_filter(mRootWindow, root_window_event_filter, this);
    g_object_unref(mRootWindow);
    mRootWindow = nullptr;
  }
}

static uint32_t GetGTKPixelDepth() {
  GdkVisual* visual = gdk_screen_get_system_visual(gdk_screen_get_default());
  return gdk_visual_get_depth(visual);
}

static already_AddRefed<Screen> MakeScreenGtk(GdkScreen* aScreen,
                                              gint aMonitorNum) {
  gint gdkScaleFactor = ScreenHelperGTK::GetGTKMonitorScaleFactor(aMonitorNum);
  // gdkScaleFactor = 2;
  // gdk_screen_get_monitor_geometry / workarea returns application pixels
  // (desktop pixels), so we need to convert it to device pixels with
  // gdkScaleFactor.
  gint geometryScaleFactor = gdkScaleFactor;

  gint refreshRate = [&] {
    // Since gtk 3.22
    static auto s_gdk_monitor_get_refresh_rate = (int (*)(GdkMonitor*))dlsym(
        RTLD_DEFAULT, "gdk_monitor_get_refresh_rate");

    if (!s_gdk_monitor_get_refresh_rate) {
      return 0;
    }
    GdkMonitor* monitor =
        GdkDisplayGetMonitor(gdk_display_get_default(), aMonitorNum);
    if (!monitor) {
      return 0;
    }
    // Convert to Hz.
    return NSToIntRound(s_gdk_monitor_get_refresh_rate(monitor) / 1000.0f);
  }();
  GdkRectangle workarea;
  gdk_screen_get_monitor_workarea(aScreen, aMonitorNum, &workarea);
  LayoutDeviceIntRect availRect(workarea.x, workarea.y, workarea.width,
                                workarea.height);
  LayoutDeviceIntRect rect;
  DesktopToLayoutDeviceScale contentsScale(1.0);
  if (GdkIsX11Display()) {
    GdkRectangle monitor;
    gdk_screen_get_monitor_geometry(aScreen, aMonitorNum, &monitor);
    rect = LayoutDeviceIntRect(monitor.x, monitor.y, monitor.width,
                               monitor.height);
  } else {
    // Don't report screen shift in Wayland, see bug 1795066.
    availRect.MoveTo(0, 0);
    // We use Gtk workarea on Wayland as it matches our needs (Bug 1732682).
    rect = availRect;
    // Use per-monitor scaling factor in Wayland.
    contentsScale.scale = gdkScaleFactor;
  }

  uint32_t pixelDepth = GetGTKPixelDepth();

  CSSToLayoutDeviceScale defaultCssScale(gdkScaleFactor);

  float dpi = 96.0f;
  gint heightMM = gdk_screen_get_monitor_height_mm(aScreen, aMonitorNum);
  if (heightMM > 0) {
    dpi = rect.height / (heightMM / MM_PER_INCH_FLOAT);
  }
  LOG_SCREEN(
      "New monitor %d size [%d,%d -> %d x %d] depth %d scale %f CssScale %f  "
      "DPI %f refresh %d ]",
      aMonitorNum, rect.x, rect.y, rect.width, rect.height, pixelDepth,
      contentsScale.scale, defaultCssScale.scale, dpi, refreshRate);
  return MakeAndAddRef<Screen>(rect, availRect, pixelDepth, pixelDepth,
                               refreshRate, contentsScale, defaultCssScale, dpi,
                               Screen::IsPseudoDisplay::No);
}

void ScreenGetterGtk::RefreshScreens() {
  LOG_SCREEN("ScreenGetterGtk::RefreshScreens()");
  AutoTArray<RefPtr<Screen>, 4> screenList;

  GdkScreen* defaultScreen = gdk_screen_get_default();
  gint numScreens = gdk_screen_get_n_monitors(defaultScreen);
  LOG_SCREEN("GDK reports %d screens", numScreens);

  for (gint i = 0; i < numScreens; i++) {
    screenList.AppendElement(MakeScreenGtk(defaultScreen, i));
  }

  ScreenManager::Refresh(std::move(screenList));
}

gint ScreenHelperGTK::GetGTKMonitorScaleFactor(gint aMonitorNum) {
  GdkScreen* screen = gdk_screen_get_default();
  return gdk_screen_get_monitor_scale_factor(screen, aMonitorNum);
}

ScreenHelperGTK::ScreenHelperGTK() {
  gScreenGetter = MakeUnique<ScreenGetterGtk>();
  gScreenGetter->Init();
}

int ScreenHelperGTK::GetMonitorCount() {
  return gdk_screen_get_n_monitors(gdk_screen_get_default());
}

ScreenHelperGTK::~ScreenHelperGTK() { gScreenGetter = nullptr; }

}  // namespace mozilla::widget
