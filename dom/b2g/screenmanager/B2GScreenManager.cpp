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
    XRROutputInfo* outputInfo = XRRGetOutputInfo(display, res, output_id);
    if (!outputInfo) continue;
    std::string name = outputInfo->name;
    if (outputInfo->connection == RR_Connected) {
      num++;
    }
    XRRFreeOutputInfo(outputInfo);
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
    XRROutputInfo* outputInfo = XRRGetOutputInfo(display, res, output_id);
    if (!outputInfo) continue;
    std::string name = outputInfo->name;
    if (outputInfo->connection == RR_Connected) {
      if (num == index) {
        index = i;
        XRRFreeOutputInfo(outputInfo);
        break;
      }
      num++;
    }
    XRRFreeOutputInfo(outputInfo);
  }
  XRROutputInfo* outputInfo =
      XRRGetOutputInfo(display, res, res->outputs[index]);
  if (!outputInfo) {
    XRRFreeScreenResources(res);
    XCloseDisplay(display);
    r.mWidth.Construct(-1);
    r.mHeight.Construct(-1);
    promise->MaybeResolve(r);
    return promise.forget();
  }

  XRRCrtcInfo* crtcInfo = XRRGetCrtcInfo(display, res, outputInfo->crtc);
  XRRModeInfo currentModeInfo;
  bool found_current = false;
  for (int i = 0; i < res->nmode; ++i) {
    if (res->modes[i].id == crtcInfo->mode) {
      currentModeInfo = res->modes[i];
      found_current = true;
      r.mWidth.Construct(res->modes[i].width);
      r.mHeight.Construct(res->modes[i].height);
      break;
    }
  }

  if (!found_current) {
    XRRFreeCrtcInfo(crtcInfo);
    XRRFreeScreenResources(res);
    XCloseDisplay(display);
    r.mWidth.Construct(-1);
    r.mHeight.Construct(-1);
    promise->MaybeResolve(r);
    return promise.forget();
  }
  XRRFreeOutputInfo(outputInfo);
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
    XRROutputInfo* outputInfo = XRRGetOutputInfo(display, res, output_id);
    if (!outputInfo) continue;
    std::string name = outputInfo->name;
    if (outputInfo->connection == RR_Connected) {
      if (num == index) {
        index = i;
        XRRFreeOutputInfo(outputInfo);
        break;
      }
      num++;
    }
    XRRFreeOutputInfo(outputInfo);
  }
  nsTArray<Resolution> resolution;

  XRROutputInfo* outputInfo =
      XRRGetOutputInfo(display, res, res->outputs[index]);
  if (!outputInfo) {
    XRRFreeScreenResources(res);
    XCloseDisplay(display);
    return promise.forget();
  }
  // 输出支持的分辨率模式
  for (int32_t m = 0; m < outputInfo->nmode; ++m) {
    XRRModeInfo* mode = &res->modes[0];
    for (int32_t j = 0; j < res->nmode; ++j) {
      if (res->modes[j].id == outputInfo->modes[m]) {
        mode = &res->modes[j];
        break;
      }
    }
    Resolution r;
    r.mWidth.Construct(mode->width);
    r.mHeight.Construct(mode->height);
    resolution.AppendElement(r);
  }

  XRRFreeOutputInfo(outputInfo);

  promise->MaybeResolve(resolution);
  XRRFreeScreenResources(res);
  XCloseDisplay(display);

  return promise.forget();
}

XRRModeInfo* FindMode(Display* dpy, XRRScreenResources* res,
                       XRROutputInfo* outputInfo, int width, int height) {
  for (int j = 0; j < res->nmode; ++j) {
    XRRModeInfo* mode = &res->modes[j];
    if (mode->width == width && mode->height == height) {
      // 检查是否支持该模式
      for (int k = 0; k < outputInfo->nmode; ++k) {
        if (outputInfo->modes[k] == mode->id) return mode;
      }
    }
  }
  return nullptr;
}

bool ExecXrandr(const std::vector<const char*>& args) {
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
std::string BuildTransform(double scaleX, int offsetX, double scaleY,
                            int offsetY) {
  char buffer[256];
  snprintf(buffer, sizeof(buffer), "%.10g,0,%d,0,%.10g,%d,0,0,1", scaleX,
           offsetX, scaleY, offsetY);
  return std::string(buffer);
}
already_AddRefed<Promise> B2GScreenManager::SetResolution(int32_t index,
                                                          int32_t displayType,
                                                          int32_t newWidth,
                                                          int32_t newHeight) {
  RefPtr<Promise> promise;
  ErrorResult rv;

  Pos p;
  promise = Promise::Create(mGlobal, rv);
  ENSURE_SUCCESS(rv, nullptr);
  Display* display = XOpenDisplay(NULL);
  Window root = DefaultRootWindow(display);
  XRRScreenResources* res = XRRGetScreenResources(display, root);
  int num = 0;
  int primaryNum = 0;
  int primaryIndex = 0;
  for (int i = 0; i < res->noutput; ++i) {
    RROutput output_id = res->outputs[i];
    XRROutputInfo* outputInfo = XRRGetOutputInfo(display, res, output_id);
    if (!outputInfo) continue;
    std::string name = outputInfo->name;
    if (outputInfo->connection == RR_Connected) {
      if (num == 0) {
        primaryNum = num;
        primaryIndex = i;
      }
      if (num == index) {
        index = i;
        XRRFreeOutputInfo(outputInfo);
        break;
      }
      num++;
    }
    XRRFreeOutputInfo(outputInfo);
  }
  XRROutputInfo* outputInfo =
      XRRGetOutputInfo(display, res, res->outputs[index]);
  if (!outputInfo) {
    p.mX.Construct(-1);
    p.mY.Construct(-1);
    promise->MaybeResolve(p);
    return promise.forget();
  }

  if (!res) {
    XRRFreeOutputInfo(outputInfo);
    XCloseDisplay(display);
    p.mX.Construct(-1);
    p.mY.Construct(-1);
    promise->MaybeResolve(p);
    return promise.forget();
  }

  XRRCrtcInfo* crtcInfo = XRRGetCrtcInfo(display, res, outputInfo->crtc);
  if (!crtcInfo) {
    XRRFreeOutputInfo(outputInfo);
    XRRFreeScreenResources(res);
    XCloseDisplay(display);
    p.mX.Construct(-1);
    p.mY.Construct(-1);
    promise->MaybeResolve(p);
    return promise.forget();
  }
  XRRModeInfo* mode =
      FindMode(display, res, outputInfo, newWidth, newHeight);
  if (!mode) {
    XRRFreeOutputInfo(outputInfo);
    XRRFreeScreenResources(res);
    XCloseDisplay(display);
    p.mX.Construct(-1);
    p.mY.Construct(-1);
    promise->MaybeResolve(p);
    return promise.forget();
  }

  if (XRRSetCrtcConfig(display, res, outputInfo->crtc, CurrentTime,
                       crtcInfo->x, crtcInfo->y, mode->id,
                       crtcInfo->rotation, &res->outputs[index], 1)) {
    XRRFreeCrtcInfo(crtcInfo);
    XRRFreeOutputInfo(outputInfo);
    XRRFreeScreenResources(res);
    XCloseDisplay(display);
    p.mX.Construct(-1);
    p.mY.Construct(-1);
    promise->MaybeResolve(p);
    return promise.forget();
  }
  if (num == primaryNum) {
    bool one_screen = true;
    for (int i = 0; i < res->noutput; ++i) {
      RROutput output = res->outputs[i];
      XRROutputInfo* outputInfo1 = XRRGetOutputInfo(display, res, output);

      if (outputInfo1->connection == RR_Connected && outputInfo1->crtc != 0 &&
          i != index) {
        one_screen = false;
        XRRCrtcInfo* crtcInfo1 =
            XRRGetCrtcInfo(display, res, outputInfo1->crtc);
        int node_index = 0;
        for (int j = 0; j < res->nmode; ++j) {
          if (res->modes[j].id == crtcInfo1->mode) {
            node_index = j;
            break;
          }
        }
        int externalWidth = res->modes[node_index].width;
        int externalHeight = res->modes[node_index].height;
        std::string pN = outputInfo->name;
        std::string oN = outputInfo1->name;
        ExecXrandr({"xrandr", "--output", oN.c_str(), "--off"});

        std::string cmd;
        if (displayType == DisplayType::MirrorReplication) {
          double scale = (double)newWidth / externalWidth;
          if (scale < (double)newHeight / externalHeight) {
            scale = (double)newHeight / externalHeight;
          }
          // 居中偏移量
          double offsetX =
              (externalWidth - newWidth / scale) / 2.0 / externalWidth;
          double offsetY =
              (externalHeight - newHeight / scale) / 2.0 / externalHeight;

          std::string transform =
              BuildTransform(scale, -offsetX * externalWidth, scale,
                              -offsetY * externalHeight);

          bool exec_succ =
              ExecXrandr({"xrandr", "--output", oN.c_str(), "--mode",
                           (std::to_string(externalWidth) + "x" +
                            std::to_string(externalHeight))
                               .c_str(),
                           "--pos", "0x0"});
          if (!exec_succ) {
            XRRFreeCrtcInfo(crtcInfo);
            XRRFreeCrtcInfo(crtcInfo1);
            XRRFreeOutputInfo(outputInfo);
            XRRFreeOutputInfo(outputInfo1);
            XRRFreeScreenResources(res);
            XCloseDisplay(display);
            p.mX.Construct(-1);
            p.mY.Construct(-1);
            promise->MaybeResolve(p);
            return promise.forget();
          }
          exec_succ = ExecXrandr({"xrandr", "--output", oN.c_str(),
                                   "--transform", transform.c_str()});

          if (!exec_succ) {
            XRRFreeCrtcInfo(crtcInfo);
            XRRFreeCrtcInfo(crtcInfo1);
            XRRFreeOutputInfo(outputInfo);
            XRRFreeOutputInfo(outputInfo1);
            XRRFreeScreenResources(res);
            XCloseDisplay(display);
            p.mX.Construct(-1);
            p.mY.Construct(-1);
            promise->MaybeResolve(p);
            return promise.forget();
          }
          p.mX.Construct(0);
          p.mY.Construct(0);
          p.mWidth.Construct(newWidth);
          p.mHeight.Construct(newHeight);
        } else if (displayType == DisplayType::Extension) {
          ExecXrandr({"xrandr", "--output", oN.c_str(), "--auto"});
          ExecXrandr({"xrandr", "--output", oN.c_str(), "--transform",
                       "1,0,0,0,1,0,0,0,1"});

          std::vector<const char*> args = {
              "xrandr",
              "--output",
              pN.c_str(),
              "--mode",
              (std::to_string(newWidth) + "x" + std::to_string(newHeight))
                  .c_str(),
              "--pos",
              "0x0",
              "--output",
              oN.c_str(),
              "--mode",
              (std::to_string(externalWidth) + "x" +
               std::to_string(externalHeight))
                  .c_str(),
              "--pos",
              (std::to_string(newWidth) + "x0").c_str(),
              "--right-of",
              pN.c_str()};
          bool exec_succ = ExecXrandr(args);
          if (!exec_succ) {
            XRRFreeCrtcInfo(crtcInfo);
            XRRFreeCrtcInfo(crtcInfo1);
            XRRFreeOutputInfo(outputInfo);
            XRRFreeOutputInfo(outputInfo1);
            XRRFreeScreenResources(res);
            XCloseDisplay(display);
            p.mX.Construct(-1);
            p.mY.Construct(-1);
            promise->MaybeResolve(p);
            return promise.forget();
          }
          p.mX.Construct(newWidth);
          p.mY.Construct(0);
          p.mWidth.Construct(externalWidth);
          p.mHeight.Construct(externalHeight);
        }
        XRRFreeCrtcInfo(crtcInfo1);
      }
      XRRFreeOutputInfo(outputInfo1);
    }
    if (one_screen) {
      p.mX.Construct(0);
      p.mY.Construct(0);
      p.mWidth.Construct(newWidth);
      p.mHeight.Construct(newHeight);
    }
  } else {
    RROutput output = res->outputs[primaryIndex];
    XRROutputInfo* outputInfo1 = XRRGetOutputInfo(display, res, output);

    if (outputInfo1->connection == RR_Connected && outputInfo1->crtc != 0) {
      XRRCrtcInfo* crtcInfo1 =
          XRRGetCrtcInfo(display, res, outputInfo1->crtc);
      int primaryWidth = crtcInfo1->width;
      int primaryHeight = crtcInfo1->height;
      std::string pN = outputInfo1->name;
      std::string oN = outputInfo->name;
      ExecXrandr({"xrandr", "--output", oN.c_str(), "--off"});

      if (displayType == DisplayType::MirrorReplication) {
        double scale = (double)primaryWidth / newWidth;
        if (scale < (double)primaryHeight / newHeight) {
          scale = (double)primaryHeight / newHeight;
        }
        double offsetX = (newWidth - primaryWidth / scale) / 2.0 / newWidth;
        double offsetY =
            (newHeight - primaryHeight / scale) / 2.0 / newHeight;

        std::string transform = BuildTransform(scale, -offsetX * newWidth,
                                                scale, -offsetY * newHeight);
        bool exec_succ = ExecXrandr(
            {"xrandr", "--output", oN.c_str(), "--mode",
             (std::to_string(newWidth) + "x" + std::to_string(newHeight))
                 .c_str(),
             "--pos", "0x0"});

        if (!exec_succ) {
          XRRFreeCrtcInfo(crtcInfo);
          XRRFreeCrtcInfo(crtcInfo1);
          XRRFreeOutputInfo(outputInfo);
          XRRFreeOutputInfo(outputInfo1);
          XRRFreeScreenResources(res);
          XCloseDisplay(display);
          p.mX.Construct(-1);
          p.mY.Construct(-1);
          promise->MaybeResolve(p);
          return promise.forget();
        }
        exec_succ = ExecXrandr({"xrandr", "--output", oN.c_str(),
                                 "--transform", transform.c_str()});

        if (!exec_succ) {
          XRRFreeCrtcInfo(crtcInfo);
          XRRFreeCrtcInfo(crtcInfo1);
          XRRFreeOutputInfo(outputInfo);
          XRRFreeOutputInfo(outputInfo1);
          XRRFreeScreenResources(res);
          XCloseDisplay(display);
          p.mX.Construct(-1);
          p.mY.Construct(-1);
          promise->MaybeResolve(p);
          return promise.forget();
        }
        p.mX.Construct(0);
        p.mY.Construct(0);
        p.mWidth.Construct(primaryWidth);
        p.mHeight.Construct(primaryHeight);
      } else if (displayType == DisplayType::Extension) {
        ExecXrandr({"xrandr", "--output", oN.c_str(), "--auto"});
        ExecXrandr({"xrandr", "--output", oN.c_str(), "--transform",
                     "1,0,0,0,1,0,0,0,1"});
        std::vector<const char*> args = {
            "xrandr",
            "--output",
            pN.c_str(),
            "--mode",
            (std::to_string(primaryWidth) + "x" +
             std::to_string(primaryHeight))
                .c_str(),
            "--pos",
            "0x0",
            "--output",
            oN.c_str(),
            "--mode",
            (std::to_string(newWidth) + "x" + std::to_string(newHeight))
                .c_str(),
            "--pos",
            (std::to_string(primaryWidth) + "x0").c_str(),
            "--right-of",
            pN.c_str()};
        bool exec_succ = ExecXrandr(args);
        if (!exec_succ) {
          XRRFreeCrtcInfo(crtcInfo);
          XRRFreeCrtcInfo(crtcInfo1);
          XRRFreeOutputInfo(outputInfo);
          XRRFreeOutputInfo(outputInfo1);
          XRRFreeScreenResources(res);
          XCloseDisplay(display);
          p.mX.Construct(-1);
          p.mY.Construct(-1);
          promise->MaybeResolve(p);
          return promise.forget();
        }
        p.mX.Construct(primaryWidth);
        p.mY.Construct(0);
        p.mWidth.Construct(newWidth);
        p.mHeight.Construct(newHeight);
      }
      XRRFreeCrtcInfo(crtcInfo1);
    }
    XRRFreeOutputInfo(outputInfo1);
  }
  XRRFreeCrtcInfo(crtcInfo);
  XRRFreeOutputInfo(outputInfo);
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
