/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set shiftwidth=2 tabstop=2 autoindent cindent expandtab: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);
const { DOMRequestHelper } = ChromeUtils.import(
  "resource://gre/modules/DOMRequestHelper.jsm"
);

const MOZ_CAMERATESTHW_CONTRACTID = "@mozilla.org/cameratesthardware;1";
const MOZ_CAMERATESTHW_CID = Components.ID(
  "{fcb7b4cd-689e-453c-8a2c-611a45fa09ac}"
);
const DEBUG = false;

function debug(msg) {
  if (DEBUG) {
    dump("-*- MozCameraTestHardware: " + msg + "\n");
  }
}

function MozCameraTestHardware() {
  this._params = {};
}

MozCameraTestHardware.prototype = {
  classID: MOZ_CAMERATESTHW_CID,
  contractID: MOZ_CAMERATESTHW_CONTRACTID,

  classInfo: XPCOMUtils.generateCI({
    classID: MOZ_CAMERATESTHW_CID,
    contractID: MOZ_CAMERATESTHW_CONTRACTID,
    flags: Ci.nsIClassInfo.SINGLETON,
    interfaces: [Ci.nsICameraTestHardware],
  }),

  QueryInterface: ChromeUtils.generateQI([Ci.nsICameraTestHardware]),

  _params: null,
  _window: null,
  _mock: null,
  _handler: null,

  attach(mock) {
    /* Waive xrays permits us to call functions provided to us
       in the mock */
    this._mock = Cu.waiveXrays(mock);
  },

  detach() {
    this._mock = null;
  },

  /* Trigger a delegate handler attached to the test hardware
     if given via attach. If there is no delegate attached, or
     it does not provide a handler for this specific operation,
     or the handler returns true, it will execute the default
     behaviour. The handler may throw an exception in order to
     return an error code from the driver call. */
  _delegate(prop) {
    return this._mock && this._mock[prop] && !this._mock[prop]();
  },

  get params() {
    return this._params;
  },

  set params(aParams) {
    this._params = aParams;
  },

  setHandler(handler) {
    this._handler = handler;
  },

  dispatchEvent(evt) {
    var self = this;
    /* We should not dispatch the event in the current thread
       context because it may be directly from a driver call
       and we could hit a deadlock situation. */
    this._window.setTimeout(function() {
      if (self._handler) {
        self._handler.handleEvent(evt);
      }
    }, 0);
  },

  reset(aWindow) {
    this._window = aWindow;
    this._mock = null;
    this._params = {};
  },

  initCamera() {
    this._delegate("init");
  },

  pushParameters(params) {
    let oldParams = this._params;
    this._params = {};
    let s = params.split(";");
    for (let i = 0; i < s.length; ++i) {
      let parts = s[i].split("=");
      if (parts.length == 2) {
        this._params[parts[0]] = parts[1];
      }
    }
    try {
      this._delegate("pushParameters");
    } catch (e) {
      this._params = oldParams;
      throw e;
    }
  },

  pullParameters() {
    this._delegate("pullParameters");
    let ret = "";
    for (let p in this._params) {
      ret += p + "=" + this._params[p] + ";";
    }
    return ret;
  },

  autoFocus() {
    if (!this._delegate("autoFocus")) {
      this.fireAutoFocusComplete(true);
    }
  },

  fireAutoFocusMoving(moving) {
    let evt = new this._window.CameraStateChangeEvent("focus", {
      newState: moving ? "focusing" : "not_focusing",
    });
    this.dispatchEvent(evt);
  },

  fireAutoFocusComplete(state) {
    let evt = new this._window.CameraStateChangeEvent("focus", {
      newState: state ? "focused" : "unfocused",
    });
    this.dispatchEvent(evt);
  },

  cancelAutoFocus() {
    this._delegate("cancelAutoFocus");
  },

  fireShutter() {
    let evt = new this._window.Event("shutter");
    this.dispatchEvent(evt);
  },

  takePicture() {
    if (!this._delegate("takePicture")) {
      this.fireTakePictureComplete(
        new this._window.Blob(["foobar"], { type: "jpeg" })
      );
    }
  },

  fireTakePictureComplete(blob) {
    let evt = new this._window.BlobEvent("picture", { data: blob });
    this.dispatchEvent(evt);
  },

  fireTakePictureError() {
    let evt = new this._window.ErrorEvent("error", { message: "picture" });
    this.dispatchEvent(evt);
  },

  cancelTakePicture() {
    this._delegate("cancelTakePicture");
  },

  startPreview() {
    this._delegate("startPreview");
  },

  stopPreview() {
    this._delegate("stopPreview");
  },

  startFaceDetection() {
    this._delegate("startFaceDetection");
  },

  stopFaceDetection() {
    this._delegate("stopFaceDetection");
  },

  fireFacesDetected(faces) {
    /* This works around the fact that we can't have references to
       dictionaries in a dictionary in WebIDL; we provide a boolean
       to indicate whether or not the values for those features are
       actually valid. */
    let facesIf = [];
    if (typeof faces === "object" && typeof faces.faces === "object") {
      let self = this;
      faces.faces.forEach(function(face) {
        face.hasLeftEye =
          face.hasOwnProperty("leftEye") && face.leftEye != null;
        face.hasRightEye =
          face.hasOwnProperty("rightEye") && face.rightEye != null;
        face.hasMouth = face.hasOwnProperty("mouth") && face.mouth != null;
        facesIf.push(new self._window.CameraDetectedFace(face));
      });
    }

    let evt = new this._window.CameraFacesDetectedEvent("facesdetected", {
      faces: facesIf,
    });
    this.dispatchEvent(evt);
  },

  startRecording() {
    this._delegate("startRecording");
  },

  stopRecording() {
    this._delegate("stopRecording");
  },

  fireSystemError() {
    let evt = new this._window.ErrorEvent("error", { message: "system" });
    this.dispatchEvent(evt);
  },
};

this.NSGetFactory = XPCOMUtils.generateNSGetFactory([MozCameraTestHardware]);
