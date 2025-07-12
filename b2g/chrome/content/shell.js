/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- /
/* vim: set shiftwidth=2 tabstop=2 autoindent cindent expandtab: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const { XPCOMUtils } = ChromeUtils.importESModule(
  "resource://gre/modules/XPCOMUtils.sys.mjs"
);
const { AppConstants } = ChromeUtils.importESModule(
  "resource://gre/modules/AppConstants.sys.mjs"
);
const { MarionetteController } = ChromeUtils.importESModule(
  "resource://gre/modules/MarionetteController.sys.mjs"
);

ChromeUtils.importESModule("resource://gre/modules/ActivitiesService.sys.mjs");
ChromeUtils.importESModule("resource://gre/modules/AlarmService.sys.mjs");
ChromeUtils.importESModule("resource://gre/modules/DownloadService.sys.mjs");
ChromeUtils.import("resource://gre/modules/NotificationDB.jsm");
ChromeUtils.importESModule("resource://gre/modules/ErrorPage.sys.mjs");
ChromeUtils.importESModule("resource://gre/modules/OrientationChangeHandler.sys.mjs");

const lazy = {};

XPCOMUtils.defineLazyGetter(this, "MarionetteHelper", () => {
  const { MarionetteHelper } = ChromeUtils.importESModule(
    "chrome://b2g/content/devtools/marionette.sys.mjs"
  );
  return new MarionetteHelper(shell.contentBrowser);
});

XPCOMUtils.defineLazyServiceGetter(
  lazy,
  "virtualcursor",
  "@mozilla.org/virtualcursor/service;1",
  "nsIVirtualCursorService"
);

ChromeUtils.defineESModuleGetters(this, {
  GeckoBridge: "resource://gre/modules/GeckoBridge.sys.mjs",
  SettingsPrefsSync: "resource://gre/modules/SettingsPrefsSync.sys.mjs",
  SafeBrowsing: "resource://gre/modules/SafeBrowsing.sys.mjs",
});

const isGonk = AppConstants.platform === "gonk";

if (isGonk) {
  ChromeUtils.importESModule(
    "resource://gre/modules/CustomHeaderInjector.sys.mjs"
  );

  XPCOMUtils.defineLazyGetter(this, "libcutils", () => {
    const { libcutils } = ChromeUtils.import(
      "resource://gre/modules/systemlibs.js"
    );
    return libcutils;
  });

  ChromeUtils.importESModule(
    "resource://gre/modules/NetworkStatsService.sys.mjs"
  );
}

try {
  // For external screen rendered by a native buffer, event of display-changed
  // (to tell a display is added), is fired after rendering the first drawble
  // frame. Load the handler asap in order to ensure our system observe that
  // event, and yes this is unfortunately a hack. So try not to delay loading
  // this module.
  if (isGonk && Services.prefs.getBoolPref("b2g.multiscreen.enabled")) {
    ChromeUtils.importESModule(
      "resource://gre/modules/MultiscreenHandler.sys.mjs"
    );
  }
} catch (e) {}

function debug(str) {
  console.log(`-*- Shell.js: ${str}`);
}

var shell = {
  get startURL() {
    let url = Services.prefs.getCharPref("b2g.system_startup_url");
    if (!url) {
      console.error(
        `Please set the b2g.system_startup_url preference properly`
      );
    }
    return url;
  },

  _started: false,
  hasStarted() {
    return this._started;
  },

  createSystemAppFrame() {
    let systemAppFrame = document.createXULElement("browser");
    systemAppFrame.setAttribute("type", "chrome");
    systemAppFrame.setAttribute("primary", "true");
    systemAppFrame.setAttribute("id", "systemapp");
    systemAppFrame.setAttribute("forcemessagemanager", "true");
    systemAppFrame.setAttribute("nodefaultsrc", "true");

    // Identify this `<browser>` element uniquely to Marionette, devtools, etc.
    systemAppFrame.permanentKey = new (Cu.getGlobalForObject(
      Services
    ).Object)();
    systemAppFrame.linkedBrowser = systemAppFrame;

    document.body.prepend(systemAppFrame);
    window.dispatchEvent(new CustomEvent("systemappframeprepended"));

    this.contentBrowser = systemAppFrame;
  },

  _progressListener: {
    stopUrl: null,

    onLocationChange: (webProgress, request, location, flags) => {
      debug(`LocationChange: ${location.spec}`);
    },

    onProgressChange: () => {
      debug(`ProgressChange`);
    },

    onSecurityChange: () => {
      debug(`SecurityChange`);
    },

    onStateChange: (webProgress, request, stateFlags, status) => {
      debug(`StateChange ${stateFlags} ${request.name}`);
      if (stateFlags & Ci.nsIWebProgressListener.STATE_START) {
        if (!this.stopUrl) {
          this.stopUrl = request.name;
        }
      }

      if (stateFlags & Ci.nsIWebProgressListener.STATE_STOP) {
        if (this.stopUrl && request.name == this.stopUrl) {
          debug(`About to notify system app is loaded.`);
          shell.contentBrowser.removeProgressListener(shell._progressListener);
          shell.notifyContentWindowLoaded();
        }
      }
    },

    onStatusChange: () => {
      debug(`StatusChange`);
    },

    QueryInterface: ChromeUtils.generateQI([
      Ci.nsIWebProgressListener2,
      Ci.nsIWebProgressListener,
      Ci.nsISupportsWeakReference,
    ]),
  },

  start() {
    if (this._started) {
      return;
    }

    this._started = true;

    let gaiaChrome = Cc["@mozilla.org/b2g/gaia-chrome;1"].getService();
    if (!gaiaChrome) {
      debug("No gaia chrome!");
    }

    // This forces the initialization of the cookie service before we hit the
    // network.
    // See bug 810209
    let cookies = Cc["@mozilla.org/cookieService;1"].getService();
    if (!cookies) {
      debug("No cookies service!");
    }

    let startURL = this.startURL;

    window.addEventListener("sizemodechange", this);
    window.addEventListener("unload", this);

    lazy.virtualcursor.init(window);

    this.contentBrowser.addProgressListener(
      this._progressListener,
      Ci.nsIWebProgress.NOTIFY_STATE_REQUEST
    );

    Services.ppmm.addMessageListener("dial-handler", this);
    Services.ppmm.addMessageListener("sms-handler", this);
    Services.ppmm.addMessageListener("mail-handler", this);
    Services.ppmm.addMessageListener("file-picker", this);

    debug(`Setting system url to ${startURL}`);

    this.contentBrowser.src = startURL;
  },

  stop() {
    window.removeEventListener("unload", this);
    window.removeEventListener("sizemodechange", this);
    Services.ppmm.removeMessageListener("dial-handler", this);
    Services.ppmm.removeMessageListener("sms-handler", this);
    Services.ppmm.removeMessageListener("mail-handler", this);
    Services.ppmm.removeMessageListener("file-picker", this);
  },

  handleEvent(event) {
    debug(`event: ${event.type}`);

    // let content = this.contentBrowser.contentWindow;
    switch (event.type) {
      case "sizemodechange":
        // Due to bug 4657, the default behavior of video/audio playing from web
        // sites should be paused when this browser tab has sent back to
        // background or phone has flip closed.
        if (window.windowState == window.STATE_MINIMIZED) {
          this.contentBrowser.docShellIsActive = false;
        } else {
          this.contentBrowser.docShellIsActive = true;
        }
        break;
      case "unload":
        this.stop();
        break;
    }
  },

  receiveMessage(message) {
    const activities = {
      "dial-handler": { name: "dial" },
      "mail-handler": { name: "new" },
      "sms-handler": { name: "new" },
      "file-picker": { name: "pick", response: "file-picked" },
    };

    if (!(message.name in activities)) {
      return;
    }

    let data = message.data;
    let activity = activities[message.name];

    let a = new window.WebActivity(activity.name, data);

    let promise = a.start();
    if (activity.response) {
      let sender = message.target;
      promise.then(
        (result) => {
          sender.sendAsyncMessage(activity.response, {
            success: true,
            result,
          });
        },
        (error) => {
          sender.sendAsyncMessage(activity.response, { success: false });
        }
      );
    }
  },

  // This gets called when window.onload fires on the System app content window,
  // which means things in <html> are parsed and statically referenced <script>s
  // and <script defer>s are loaded and run.
  notifyContentWindowLoaded() {
    debug("notifyContentWindowLoaded");
    if (isGonk) {
      libcutils.property_set("shell.ready", "1");
    }
    if (this.contentBrowser.getAttribute("kind") == "touch") {
      this.contentBrowser.classList.add("fullscreen");
      this.contentBrowser.removeAttribute("style");
    }
    // This will cause Gonk Widget to remove boot animation from the screen
    // and reveals the page.
    Services.obs.notifyObservers(null, "browser-ui-startup-complete");

    // Needed to kick off WebExtension background pages.
    Services.obs.notifyObservers(window, "browser-delayed-startup-finished");
    Services.obs.notifyObservers(window, "extensions-late-startup");
  },
};

function toggle_bool_pref(name) {
  let current = Services.prefs.getBoolPref(name);
  Services.prefs.setBoolPref(name, !current);
  debug(`${name} is now ${!current}`);
}

function debug_keyevent(evt) {
  let msg = "type: " + evt.type + ", key: " + evt.key;
  msg += ", target: " + evt.target;
  if (evt.target.tagName) {
    msg += ", tag: " + evt.target.tagName;
  }
  if (evt.target.id) {
    msg += ", id: " + evt.target.id;
  }
  if (evt.target.className) {
    msg += ", class: " + evt.target.className;
  }
  if (evt.target.src) {
    msg += ", src: " + evt.target.src;
  }
  debug(`${msg}`);
}

document.addEventListener("keydown", debug_keyevent);

document.addEventListener("keyup", debug_keyevent);

document.addEventListener(
  "DOMContentLoaded",
  () => {
    if (shell.hasStarted()) {
      // Should never happen!
      console.error("Shell has already started but didn't initialize!!!");
      return;
    }

    // eslint-disable-next-line no-undef
    RemoteDebugger.init(window);

    Services.obs.addObserver((browserWindowImpl) => {
      debug("New web embedder created.");
      window.browserDOMWindow = browserWindowImpl;

      // Notify the the shell is ready at the next event loop tick to
      // let the embedder user a chance to add event listeners.
      window.setTimeout(() => {
        // Provide a sync accessor to the shell readiness.
        shell.isReady = true;
        Services.obs.notifyObservers(window, "shell-ready");
      }, 0);
    }, "web-embedder-created");

    // Always initialize Marionette server in userdebug and desktop builds
    if (!isGonk || libcutils.property_get("ro.build.type") == "userdebug") {
      Services.tm.idleDispatchToMainThread(() => {
        Services.obs.notifyObservers(null, "browser-delayed-startup-finished");
        Services.obs.notifyObservers(
          null,
          "browser-idle-startup-tasks-finished"
        );
      });
    }

    // Use another way to initialize Marionette server in user builds
    if (isGonk && libcutils.property_get("ro.build.type") == "user") {
      if (MarionetteController) {
        MarionetteController.enableRunner();
      } else {
        console.warn("MarionetteController not exist");
      }
    }

    // Wait for the the on-boot-done event to start the shell.
    // If the on-boot-done is not reported,
    // the shell will be started by the following timeout handler.
    let onBootDone = new Promise((resolve) => {
      Services.obs.addObserver(() => {
        resolve();
      }, "on-boot-done");
    });

    // Start the SIDL <-> Gecko bridge.
    GeckoBridge.start();
    // Init SafeBrowsing prefs.
    SafeBrowsing.init();

    shell.createSystemAppFrame();

    // Start the Settings <-> Preferences synchronizer.
    let languageReady = SettingsPrefsSync.start(window).then(() => {
      // TODO: check if there is a better time to run delayedInit()
      // for the overall OS startup, like when the homescreen is ready.
      window.setTimeout(() => {
        SettingsPrefsSync.delayedInit();
      }, 10000);
    });

    // As soon as apps boot is done and the locale is configured,
    // start loading the system app.
    Promise.allSettled([onBootDone, languageReady]).then(() => {
      shell.start();
    });

    // Force startup if we didn't get the settings ready under 3s.
    // That can happen in configurations without the api-daemon.
    window.setTimeout(() => {
      shell.start();
    }, 3000);
  },
  { once: true }
);
