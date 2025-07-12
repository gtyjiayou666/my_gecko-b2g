// -*- Mode: js; tab-width: 2; indent-tabs-mode: nil; js2-basic-offset: 2; js2-skip-preprocessor-directives: t; -*-
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

/* TODO: proper cleanup */
/* globals lazy, XPCOMUtils, Services, Prompt, Strings, WindowEventDispatcher */

"use strict";

XPCOMUtils.defineLazyGetter(lazy, "require", () => {
  let { require } = ChromeUtils.importESModule(
    "resource://devtools/shared/loader/Loader.sys.mjs"
  );
  return require;
});

XPCOMUtils.defineLazyGetter(lazy, "DevToolsServer", () => {
  let { DevToolsServer } = lazy.require("devtools/server/devtools-server");
  return DevToolsServer;
});

XPCOMUtils.defineLazyGetter(lazy, "SocketListener", () => {
  let { SocketListener } = lazy.require("devtools/shared/security/socket");
  return SocketListener;
});

var RemoteDebugger = {
  init(aWindow) {
    this._windowType = "navigator:browser";

    USBRemoteDebugger.init();
    WiFiRemoteDebugger.init();

    const listener = event => {
      if (event.target !== aWindow) {
        return;
      }

      if (this.isAnyEnabled) {
        this.initServer();
      }
    };
    aWindow.addEventListener("activate", listener, { mozSystemGroup: true });
    aWindow.addEventListener("deactivate", listener, { mozSystemGroup: true });
  },

  get isAnyEnabled() {
    return USBRemoteDebugger.isEnabled || WiFiRemoteDebugger.isEnabled;
  },

  /**
   * Prompt the user to accept or decline the incoming connection.
   *
   * @param {object} session
   *        The session object will contain at least the following fields:
   *        {
   *          authentication,
   *          client: {
   *            host,
   *            port
   *          },
   *          server: {
   *            host,
   *            port
   *          }
   *        }
   *        Specific authentication modes may include additional fields.  Check
   *        the different |allowConnection| methods in
   *        devtools/shared/security/auth.js.
   * @returns {AuthenticationResult} value.
   *         A promise that will be resolved to the above is also allowed.
   */
  allowConnection(session) {
    if (this._promptingForAllow) {
      // Don't stack connection prompts if one is already open
      return lazy.DevToolsServer.AuthenticationResult.DENY;
    }

    if (!session.server.port) {
      this._promptingForAllow = this._promptForUSB(session);
    } else {
      this._promptingForAllow = this._promptForTCP(session);
    }
    this._promptingForAllow.then(() => (this._promptingForAllow = null));

    return this._promptingForAllow;
  },

  _promptForUSB(session) {
    if (session.authentication !== "PROMPT") {
      // This dialog is not prepared for any other authentication method at
      // this time.
      return lazy.DevToolsServer.AuthenticationResult.DENY;
    }

    return new Promise(resolve => {
      let title = Strings.browser.GetStringFromName(
        "remoteIncomingPromptTitle"
      );
      let msg = Strings.browser.GetStringFromName("remoteIncomingPromptUSB");
      let allow = Strings.browser.GetStringFromName(
        "remoteIncomingPromptAllow"
      );
      let deny = Strings.browser.GetStringFromName("remoteIncomingPromptDeny");

      // Make prompt. Note: button order is in reverse.
      let prompt = new Prompt({
        window: null,
        hint: "remotedebug",
        title,
        message: msg,
        buttons: [allow, deny],
        priority: 1,
      });

      prompt.show(data => {
        let result = data.button;
        if (result === 0) {
          resolve(lazy.DevToolsServer.AuthenticationResult.ALLOW);
        } else {
          resolve(lazy.DevToolsServer.AuthenticationResult.DENY);
        }
      });
    });
  },

  _promptForTCP(session) {
    if (session.authentication !== "OOB_CERT" || !session.client.cert) {
      // This dialog is not prepared for any other authentication method at
      // this time.
      return lazy.DevToolsServer.AuthenticationResult.DENY;
    }

    return new Promise(resolve => {
      let title = Strings.browser.GetStringFromName(
        "remoteIncomingPromptTitle"
      );
      let msg = Strings.browser.formatStringFromName(
        "remoteIncomingPromptTCP",
        [session.client.host, session.client.port]
      );
      let scan = Strings.browser.GetStringFromName("remoteIncomingPromptScan");
      let scanAndRemember = Strings.browser.GetStringFromName(
        "remoteIncomingPromptScanAndRemember"
      );
      let deny = Strings.browser.GetStringFromName("remoteIncomingPromptDeny");

      // Make prompt. Note: button order is in reverse.
      let prompt = new Prompt({
        window: null,
        hint: "remotedebug",
        title,
        message: msg,
        buttons: [scan, scanAndRemember, deny],
        priority: 1,
      });

      prompt.show(data => {
        let result = data.button;
        if (result === 0) {
          resolve(lazy.DevToolsServer.AuthenticationResult.ALLOW);
        } else if (result === 1) {
          resolve(lazy.DevToolsServer.AuthenticationResult.ALLOW_PERSIST);
        } else {
          resolve(lazy.DevToolsServer.AuthenticationResult.DENY);
        }
      });
    });
  },

  /**
   * During OOB_CERT authentication, the user must transfer some data through
   * some out of band mechanism from the client to the server to authenticate
   * the devices.
   *
   * This implementation instructs Fennec to invoke a QR decoder and return the
   * the data it contains back here.
   *
   * @returns {object} containing:
   *         sha256: hash(ClientCert)
   *         k     : K(random 128-bit number)
   *         A promise that will be resolved to the above is also allowed.
   */
  receiveOOB() {
    if (this._receivingOOB) {
      return this._receivingOOB;
    }

    this._receivingOOB = WindowEventDispatcher.sendRequestForResult({
      type: "DevToolsAuth:Scan",
    }).then(
      data => {
        return JSON.parse(data);
      },
      () => {
        let title = Strings.browser.GetStringFromName(
          "remoteQRScanFailedPromptTitle"
        );
        let msg = Strings.browser.GetStringFromName(
          "remoteQRScanFailedPromptMessage"
        );
        let ok = Strings.browser.GetStringFromName(
          "remoteQRScanFailedPromptOK"
        );
        let prompt = new Prompt({
          window: null,
          hint: "remotedebug",
          title,
          message: msg,
          buttons: [ok],
          priority: 1,
        });
        prompt.show();
      }
    );

    this._receivingOOB.then(() => (this._receivingOOB = null));

    return this._receivingOOB;
  },

  initServer() {
    lazy.DevToolsServer.init();

    // Add browser and Fennec specific actors
    lazy.DevToolsServer.registerAllActors();
    const { createRootActor } = lazy.require(
      "resource://gre/modules/dbg-browser-actors.js"
    );
    lazy.DevToolsServer.setRootActor(createRootActor);

    // Allow debugging of chrome for any process
    lazy.DevToolsServer.allowChromeProcess = true;
    lazy.DevToolsServer.chromeWindowType = this._windowType;
    // Force the Server to stay alive even if there are no connections at the moment.
    lazy.DevToolsServer.keepAlive = true;
  },
};

RemoteDebugger.allowConnection =
  RemoteDebugger.allowConnection.bind(RemoteDebugger);
RemoteDebugger.receiveOOB = RemoteDebugger.receiveOOB.bind(RemoteDebugger);

var USBRemoteDebugger = {
  init() {
    Services.prefs.addObserver("devtools.", this);

    if (this.isEnabled) {
      this.start();
    }
  },

  observe(subject, topic, data) {
    if (topic != "nsPref:changed") {
      return;
    }

    switch (data) {
      case "devtools.remote.usb.enabled":
        Services.prefs.setBoolPref(
          "devtools.debugger.remote-enabled",
          RemoteDebugger.isAnyEnabled
        );
        if (this.isEnabled) {
          this.start();
        } else {
          this.stop();
        }
        break;

      case "devtools.debugger.remote-port":
      case "devtools.debugger.unix-domain-socket":
        if (this.isEnabled) {
          this.stop();
          this.start();
        }
        break;
    }
  },

  get isEnabled() {
    return Services.prefs.getBoolPref("devtools.remote.usb.enabled");
  },

  start() {
    if (this._listener) {
      return;
    }

    RemoteDebugger.initServer();

    const portOrPath =
      Services.prefs.getCharPref("devtools.debugger.unix-domain-socket") ||
      Services.prefs.getIntPref("devtools.debugger.remote-port");

    try {
      dump(`Starting USB debugger on ${portOrPath}\n`);
      const AuthenticatorType =
        lazy.DevToolsServer.Authenticators.get("PROMPT");
      const authenticator = new AuthenticatorType.Server();
      authenticator.allowConnection = RemoteDebugger.allowConnection;
      const socketOptions = { authenticator, portOrPath };
      this._listener = new lazy.SocketListener(
        lazy.DevToolsServer,
        socketOptions
      );
      this._listener.open();
    } catch (e) {
      dump("Unable to start USB debugger server: " + e);
    }
  },

  stop() {
    if (!this._listener) {
      return;
    }

    try {
      this._listener.close();
      this._listener = null;
    } catch (e) {
      dump("Unable to stop USB debugger server: " + e);
    }
  },
};

var WiFiRemoteDebugger = {
  init() {
    Services.prefs.addObserver("devtools.", this);

    if (this.isEnabled) {
      this.start();
    }
  },

  observe(subject, topic, data) {
    if (topic != "nsPref:changed") {
      return;
    }

    switch (data) {
      case "devtools.remote.wifi.enabled":
        Services.prefs.setBoolPref(
          "devtools.debugger.remote-enabled",
          RemoteDebugger.isAnyEnabled
        );
        // Allow remote debugging on non-local interfaces when WiFi debug is
        // enabled
        // TODO: Bug 1034411: Lock down to WiFi interface only
        Services.prefs.setBoolPref(
          "devtools.debugger.force-local",
          !this.isEnabled
        );
        if (this.isEnabled) {
          this.start();
        } else {
          this.stop();
        }
        break;
    }
  },

  get isEnabled() {
    return Services.prefs.getBoolPref("devtools.remote.wifi.enabled");
  },

  start() {
    if (this._listener) {
      return;
    }

    RemoteDebugger.initServer();

    try {
      dump("Starting WiFi debugger");
      const AuthenticatorType =
        lazy.DevToolsServer.Authenticators.get("OOB_CERT");
      const authenticator = new AuthenticatorType.Server();
      authenticator.allowConnection = RemoteDebugger.allowConnection;
      authenticator.receiveOOB = RemoteDebugger.receiveOOB;
      const socketOptions = {
        authenticator,
        discoverable: true,
        encryption: true,
        portOrPath: -1,
      };
      this._listener = new lazy.SocketListener(
        lazy.DevToolsServer,
        socketOptions
      );
      this._listener.open();
      let port = this._listener.port;
      dump("Started WiFi debugger on " + port);
    } catch (e) {
      dump("Unable to start WiFi debugger server: " + e);
    }
  },

  stop() {
    if (!this._listener) {
      return;
    }

    try {
      this._listener.close();
      this._listener = null;
    } catch (e) {
      dump("Unable to stop WiFi debugger server: " + e);
    }
  },
};
