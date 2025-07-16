/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "B2GScreenManager.h"
#include "nsIGlobalObject.h"
#include "mozilla/widget/ScreenManager.h"
#include "mozilla/dom/Promise.h"
#include <iostream>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <sys/wait.h>
using namespace mozilla::widget;

namespace mozilla {
namespace dom {

B2GScreenManager::B2GScreenManager(nsIGlobalObject* aGlobal)
    : mGlobal(aGlobal) {
  MOZ_ASSERT(aGlobal);
  // Init();
}
// void B2GScreenManager::Init() {
// }

already_AddRefed<Promise> B2GScreenManager::GetScreenNum() {
  RefPtr<Promise> promise;
  ErrorResult rv;
  promise = Promise::Create(mGlobal, rv);
  ENSURE_SUCCESS(rv, nullptr);

  Display* display = XOpenDisplay(NULL);
  Window root = DefaultRootWindow(display);

  XRRScreenResources* res = XRRGetScreenResources(display, root);
  int num = 0;
  for (int i = 0; i < res->noutput; ++i) {
    RROutput output_id = res->outputs[i];
    XRROutputInfo* output_info = XRRGetOutputInfo(display, res, output_id);
    if (!output_info) continue;
    std::string name = output_info->name;
    if (output_info->connection == RR_Connected) {
      num++;
    }
    XRRFreeOutputInfo(output_info);
  }

  promise->MaybeResolve(num);
  XRRFreeScreenResources(res);
  XCloseDisplay(display);
  // promise->MaybeResolve(ScreenManager::GetSingleton().CurrentScreenList().Length());

  return promise.forget();
}

already_AddRefed<Promise> B2GScreenManager::GetCurrentResolution(
    int32_t index) {
  RefPtr<Promise> promise;
  ErrorResult rv;
  promise = Promise::Create(mGlobal, rv);
  ENSURE_SUCCESS(rv, nullptr);
  Resolution r;

  Display* display = XOpenDisplay(NULL);
  Window root = DefaultRootWindow(display);

  XRRScreenResources* res = XRRGetScreenResources(display, root);
  int num = 0;
  for (int i = 0; i < res->noutput; ++i) {
    RROutput output_id = res->outputs[i];
    XRROutputInfo* output_info = XRRGetOutputInfo(display, res, output_id);
    if (!output_info) continue;
    std::string name = output_info->name;
    if (output_info->connection == RR_Connected) {
      if (num == index) {
        index = i;
        XRRFreeOutputInfo(output_info);
        break;
      }
      num++;
    }
    XRRFreeOutputInfo(output_info);
  }
  XRROutputInfo* output_info =
      XRRGetOutputInfo(display, res, res->outputs[index]);
  if (!output_info) {
    XRRFreeScreenResources(res);
    XCloseDisplay(display);
    r.mWidth.Construct(-1);
    r.mHeight.Construct(-1);
    promise->MaybeResolve(r);
    return promise.forget();
  }

  XRRCrtcInfo* crtc_info = XRRGetCrtcInfo(display, res, output_info->crtc);
  XRRModeInfo current_mode_info;
  bool found_current = false;
  for (int i = 0; i < res->nmode; ++i) {
    if (res->modes[i].id == crtc_info->mode) {
      current_mode_info = res->modes[i];
      found_current = true;
      r.mWidth.Construct(res->modes[i].width);
      r.mHeight.Construct(res->modes[i].height);
      break;
    }
  }

  if (!found_current) {
    XRRFreeCrtcInfo(crtc_info);
    XRRFreeScreenResources(res);
    XCloseDisplay(display);
    r.mWidth.Construct(-1);
    r.mHeight.Construct(-1);
    promise->MaybeResolve(r);
    return promise.forget();
  }
  XRRFreeOutputInfo(output_info);
  promise->MaybeResolve(r);
  XRRFreeScreenResources(res);
  XCloseDisplay(display);

  return promise.forget();
}

already_AddRefed<Promise> B2GScreenManager::GetScreenResolutions(
    int32_t index) {
  RefPtr<Promise> promise;
  ErrorResult rv;
  promise = Promise::Create(mGlobal, rv);
  ENSURE_SUCCESS(rv, nullptr);

  Display* display = XOpenDisplay(NULL);
  Window root = DefaultRootWindow(display);

  XRRScreenResources* res = XRRGetScreenResources(display, root);
  int num = 0;
  for (int i = 0; i < res->noutput; ++i) {
    RROutput output_id = res->outputs[i];
    XRROutputInfo* output_info = XRRGetOutputInfo(display, res, output_id);
    if (!output_info) continue;
    std::string name = output_info->name;
    if (output_info->connection == RR_Connected) {
      if (num == index) {
        index = i;
        XRRFreeOutputInfo(output_info);
        break;
      }
      num++;
    }
    XRRFreeOutputInfo(output_info);
  }
  nsTArray<Resolution> resolution;

  XRROutputInfo* output_info =
      XRRGetOutputInfo(display, res, res->outputs[index]);
  if (!output_info) {
    XRRFreeScreenResources(res);
    XCloseDisplay(display);
    return promise.forget();
  }
  // 输出支持的分辨率模式
  for (int32_t m = 0; m < output_info->nmode; ++m) {
    XRRModeInfo* mode = &res->modes[0];
    for (int32_t j = 0; j < res->nmode; ++j) {
      if (res->modes[j].id == output_info->modes[m]) {
        mode = &res->modes[j];
        break;
      }
    }
    Resolution r;
    r.mWidth.Construct(mode->width);
    r.mHeight.Construct(mode->height);
    resolution.AppendElement(r);
  }

  XRRFreeOutputInfo(output_info);

  promise->MaybeResolve(resolution);
  XRRFreeScreenResources(res);
  XCloseDisplay(display);

  return promise.forget();
}

XRRModeInfo* find_mode(Display* dpy, XRRScreenResources* res,
                       XRROutputInfo* output_info, int width, int height) {
  for (int j = 0; j < res->nmode; ++j) {
    XRRModeInfo* mode = &res->modes[j];
    if (mode->width == width && mode->height == height) {
      // 检查是否支持该模式
      for (int k = 0; k < output_info->nmode; ++k) {
        if (output_info->modes[k] == mode->id) return mode;
      }
    }
  }
  return nullptr;
}

bool exec_xrandr(const std::vector<const char*>& args) {
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
std::string build_transform(double scale_x, int offset_x, double scale_y,
                            int offset_y) {
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "%.10g,0,%d,0,%.10g,%d,0,0,1", scale_x,
           offset_x, scale_y, offset_y);
  return std::string(buffer);
}
already_AddRefed<Promise> B2GScreenManager::SetResolution(int32_t screen_num,
                                                          int32_t extension_mod,
                                                          int32_t new_width,
                                                          int32_t new_height) {
  RefPtr<Promise> promise;
  ErrorResult rv;

  Pos p;
  promise = Promise::Create(mGlobal, rv);
  ENSURE_SUCCESS(rv, nullptr);
  Display* display = XOpenDisplay(NULL);
  Window root = DefaultRootWindow(display);
  XRRScreenResources* res = XRRGetScreenResources(display, root);
  int num = 0;
  int primary_num = 0;
  int primary_index = 0;
  for (int i = 0; i < res->noutput; ++i) {
    RROutput output_id = res->outputs[i];
    XRROutputInfo* output_info = XRRGetOutputInfo(display, res, output_id);
    if (!output_info) continue;
    std::string name = output_info->name;
    if (output_info->connection == RR_Connected) {
      if (num == 0) {
        primary_num = num;
        primary_index = i;
      }
      if (num == screen_num) {
        screen_num = i;
        XRRFreeOutputInfo(output_info);
        break;
      }
      num++;
    }
    XRRFreeOutputInfo(output_info);
  }
  XRROutputInfo* output_info =
      XRRGetOutputInfo(display, res, res->outputs[screen_num]);
  if (!output_info) {
    p.mX.Construct(-1);
    p.mY.Construct(-1);
    promise->MaybeResolve(p);
    return promise.forget();
  }

  if (!res) {
    XRRFreeOutputInfo(output_info);
    XCloseDisplay(display);
    p.mX.Construct(-1);
    p.mY.Construct(-1);
    promise->MaybeResolve(p);
    return promise.forget();
  }

  XRRCrtcInfo* crtc_info = XRRGetCrtcInfo(display, res, output_info->crtc);
  if (!crtc_info) {
    XRRFreeOutputInfo(output_info);
    XRRFreeScreenResources(res);
    XCloseDisplay(display);
    p.mX.Construct(-1);
    p.mY.Construct(-1);
    promise->MaybeResolve(p);
    return promise.forget();
  }
  XRRModeInfo* mode =
      find_mode(display, res, output_info, new_width, new_height);
  if (!mode) {
    XRRFreeOutputInfo(output_info);
    XRRFreeScreenResources(res);
    XCloseDisplay(display);
    p.mX.Construct(-1);
    p.mY.Construct(-1);
    promise->MaybeResolve(p);
    return promise.forget();
  }

  if (XRRSetCrtcConfig(display, res, output_info->crtc, CurrentTime,
                       crtc_info->x, crtc_info->y, mode->id,
                       crtc_info->rotation, &res->outputs[screen_num], 1)) {
    XRRFreeCrtcInfo(crtc_info);
    XRRFreeOutputInfo(output_info);
    XRRFreeScreenResources(res);
    XCloseDisplay(display);
    p.mX.Construct(-1);
    p.mY.Construct(-1);
    promise->MaybeResolve(p);
    return promise.forget();
  }
  if (num == primary_num) {
    bool one_screen = true;
    for (int i = 0; i < res->noutput; ++i) {
      RROutput output = res->outputs[i];
      XRROutputInfo* output_info1 = XRRGetOutputInfo(display, res, output);

      if (output_info1->connection == RR_Connected && output_info1->crtc != 0 &&
          i != screen_num) {
        one_screen = false;
        XRRCrtcInfo* crtc_info1 =
            XRRGetCrtcInfo(display, res, output_info1->crtc);
        int node_index = 0;
        for (int j = 0; j < res->nmode; ++j) {
          if (res->modes[j].id == crtc_info1->mode) {
            node_index = j;
            break;
          }
        }
        int external_width = res->modes[node_index].width;
        int external_height = res->modes[node_index].height;
        std::string p_n = output_info->name;
        std::string o_n = output_info1->name;
        exec_xrandr({"xrandr", "--output", o_n.c_str(), "--off"});

        std::string cmd;
        if (extension_mod == 0) {
          double scale = (double)new_width / external_width;
          if (scale < (double)new_height / external_height) {
            scale = (double)new_height / external_height;
          }
          // 居中偏移量
          double offset_x =
              (external_width - new_width / scale) / 2.0 / external_width;
          double offset_y =
              (external_height - new_height / scale) / 2.0 / external_height;

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
            XRRFreeCrtcInfo(crtc_info);
            XRRFreeCrtcInfo(crtc_info1);
            XRRFreeOutputInfo(output_info);
            XRRFreeOutputInfo(output_info1);
            XRRFreeScreenResources(res);
            XCloseDisplay(display);
            p.mX.Construct(-1);
            p.mY.Construct(-1);
            promise->MaybeResolve(p);
            return promise.forget();
          }
          exec_succ = exec_xrandr({"xrandr", "--output", o_n.c_str(),
                                   "--transform", transform.c_str()});

          if (!exec_succ) {
            XRRFreeCrtcInfo(crtc_info);
            XRRFreeCrtcInfo(crtc_info1);
            XRRFreeOutputInfo(output_info);
            XRRFreeOutputInfo(output_info1);
            XRRFreeScreenResources(res);
            XCloseDisplay(display);
            p.mX.Construct(-1);
            p.mY.Construct(-1);
            promise->MaybeResolve(p);
            return promise.forget();
          }
          p.mX.Construct(0);
          p.mY.Construct(0);
          p.mWidth.Construct(new_width);
          p.mHeight.Construct(new_height);
        } else {
          exec_xrandr({"xrandr", "--output", o_n.c_str(), "--auto"});
          exec_xrandr({"xrandr", "--output", o_n.c_str(), "--transform",
                       "1,0,0,0,1,0,0,0,1"});

          std::vector<const char*> args = {
              "xrandr",
              "--output",
              p_n.c_str(),
              "--mode",
              (std::to_string(new_width) + "x" + std::to_string(new_height))
                  .c_str(),
              "--pos",
              "0x0",
              "--output",
              o_n.c_str(),
              "--mode",
              (std::to_string(external_width) + "x" +
               std::to_string(external_height))
                  .c_str(),
              "--pos",
              (std::to_string(new_width) + "x0").c_str(),
              "--right-of",
              p_n.c_str()};
          bool exec_succ = exec_xrandr(args);
          if (!exec_succ) {
            XRRFreeCrtcInfo(crtc_info);
            XRRFreeCrtcInfo(crtc_info1);
            XRRFreeOutputInfo(output_info);
            XRRFreeOutputInfo(output_info1);
            XRRFreeScreenResources(res);
            XCloseDisplay(display);
            p.mX.Construct(-1);
            p.mY.Construct(-1);
            promise->MaybeResolve(p);
            return promise.forget();
          }
          p.mX.Construct(new_width);
          p.mY.Construct(0);
          p.mWidth.Construct(external_width);
          p.mHeight.Construct(external_height);
        }
        XRRFreeCrtcInfo(crtc_info1);
      }
      XRRFreeOutputInfo(output_info1);
    }
    if (one_screen) {
      p.mX.Construct(0);
      p.mY.Construct(0);
      p.mWidth.Construct(new_width);
      p.mHeight.Construct(new_height);
    }
  } else {
    RROutput output = res->outputs[primary_index];
    XRROutputInfo* output_info1 = XRRGetOutputInfo(display, res, output);

    if (output_info1->connection == RR_Connected && output_info1->crtc != 0) {
      XRRCrtcInfo* crtc_info1 =
          XRRGetCrtcInfo(display, res, output_info1->crtc);
      int primary_width = crtc_info1->width;
      int primary_height = crtc_info1->height;
      std::string p_n = output_info1->name;
      std::string o_n = output_info->name;
      exec_xrandr({"xrandr", "--output", o_n.c_str(), "--off"});

      if (extension_mod == 0) {
        double scale = (double)primary_width / new_width;
        if (scale < (double)primary_height / new_height) {
          scale = (double)primary_height / new_height;
        }
        double offset_x = (new_width - primary_width / scale) / 2.0 / new_width;
        double offset_y =
            (new_height - primary_height / scale) / 2.0 / new_height;

        std::string transform = build_transform(scale, -offset_x * new_width,
                                                scale, -offset_y * new_height);
        bool exec_succ = exec_xrandr(
            {"xrandr", "--output", o_n.c_str(), "--mode",
             (std::to_string(new_width) + "x" + std::to_string(new_height))
                 .c_str(),
             "--pos", "0x0"});

        if (!exec_succ) {
          XRRFreeCrtcInfo(crtc_info);
          XRRFreeCrtcInfo(crtc_info1);
          XRRFreeOutputInfo(output_info);
          XRRFreeOutputInfo(output_info1);
          XRRFreeScreenResources(res);
          XCloseDisplay(display);
          p.mX.Construct(-1);
          p.mY.Construct(-1);
          promise->MaybeResolve(p);
          return promise.forget();
        }
        exec_succ = exec_xrandr({"xrandr", "--output", o_n.c_str(),
                                 "--transform", transform.c_str()});

        if (!exec_succ) {
          XRRFreeCrtcInfo(crtc_info);
          XRRFreeCrtcInfo(crtc_info1);
          XRRFreeOutputInfo(output_info);
          XRRFreeOutputInfo(output_info1);
          XRRFreeScreenResources(res);
          XCloseDisplay(display);
          p.mX.Construct(-1);
          p.mY.Construct(-1);
          promise->MaybeResolve(p);
          return promise.forget();
        }
        p.mX.Construct(0);
        p.mY.Construct(0);
        p.mWidth.Construct(primary_width);
        p.mHeight.Construct(primary_height);
      } else {
        exec_xrandr({"xrandr", "--output", o_n.c_str(), "--auto"});
        exec_xrandr({"xrandr", "--output", o_n.c_str(), "--transform",
                     "1,0,0,0,1,0,0,0,1"});
        std::vector<const char*> args = {
            "xrandr",
            "--output",
            p_n.c_str(),
            "--mode",
            (std::to_string(primary_width) + "x" +
             std::to_string(primary_height))
                .c_str(),
            "--pos",
            "0x0",
            "--output",
            o_n.c_str(),
            "--mode",
            (std::to_string(new_width) + "x" + std::to_string(new_height))
                .c_str(),
            "--pos",
            (std::to_string(primary_width) + "x0").c_str(),
            "--right-of",
            p_n.c_str()};
        bool exec_succ = exec_xrandr(args);
        if (!exec_succ) {
          XRRFreeCrtcInfo(crtc_info);
          XRRFreeCrtcInfo(crtc_info1);
          XRRFreeOutputInfo(output_info);
          XRRFreeOutputInfo(output_info1);
          XRRFreeScreenResources(res);
          XCloseDisplay(display);
          p.mX.Construct(-1);
          p.mY.Construct(-1);
          promise->MaybeResolve(p);
          return promise.forget();
        }
        p.mX.Construct(primary_width);
        p.mY.Construct(0);
        p.mWidth.Construct(new_width);
        p.mHeight.Construct(new_height);
      }
      XRRFreeCrtcInfo(crtc_info1);
    }
    XRRFreeOutputInfo(output_info1);
  }
  XRRFreeCrtcInfo(crtc_info);
  XRRFreeOutputInfo(output_info);
  XRRFreeScreenResources(res);
  XCloseDisplay(display);
  promise->MaybeResolve(p);
  return promise.forget();
}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(B2GScreenManager, mGlobal)
NS_IMPL_CYCLE_COLLECTING_ADDREF(B2GScreenManager)
NS_IMPL_CYCLE_COLLECTING_RELEASE(B2GScreenManager)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(B2GScreenManager)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

JSObject* B2GScreenManager::WrapObject(JSContext* aCx,
                                       JS::Handle<JSObject*> aGivenProto) {
  return B2GScreenManager_Binding::Wrap(aCx, this, aGivenProto);
}

}  // namespace dom
}  // namespace mozilla
