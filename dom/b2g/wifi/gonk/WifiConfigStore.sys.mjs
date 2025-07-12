/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { FileUtils } from "resource://gre/modules/FileUtils.sys.mjs";

const { NetUtil } = ChromeUtils.import("resource://gre/modules/NetUtil.jsm");

var gDebug = false;

export const WifiConfigStore = (function () {
  var wifiConfigStore = {};

  const WIFI_CONFIG_PATH = "/data/misc/wifi/wifi_config.json";
  const PASSPOINT_CONFIG_PATH = "/data/misc/wifi/passpoint_config.json";

  // WifiConfigStore functions
  wifiConfigStore.WIFI_CONFIG_PATH = WIFI_CONFIG_PATH;
  wifiConfigStore.PASSPOINT_CONFIG_PATH = PASSPOINT_CONFIG_PATH;
  wifiConfigStore.read = read;
  wifiConfigStore.write = write;
  wifiConfigStore.setDebug = setDebug;

  function setDebug(aDebug) {
    gDebug = aDebug;
  }

  function debug(aMsg) {
    if (gDebug) {
      dump("-*- WifiConfigStore: " + aMsg);
    }
  }

  function read(path) {
    let configFile = new FileUtils.File(path);
    let configuration;
    if (configFile.exists()) {
      let fstream = Cc[
        "@mozilla.org/network/file-input-stream;1"
      ].createInstance(Ci.nsIFileInputStream);
      let sstream = Cc["@mozilla.org/scriptableinputstream;1"].createInstance(
        Ci.nsIScriptableInputStream
      );
      let converter = Cc[
        "@mozilla.org/intl/scriptableunicodeconverter"
      ].createInstance(Ci.nsIScriptableUnicodeConverter);
      converter.charset = "UTF-8";
      const RO = 0x01;
      const READ_OTHERS = 4;

      fstream.init(configFile, RO, READ_OTHERS, 0);
      sstream.init(fstream);
      let data = sstream.read(sstream.available());
      sstream.close();
      fstream.close();
      configuration = JSON.parse(converter.ConvertToUnicode(data));
    } else {
      configuration = null;
    }
    return configuration;
  }

  function write(path, data, callback) {
    let configFile = new FileUtils.File(path);
    // Initialize the file output stream.
    let ostream = FileUtils.openSafeFileOutputStream(configFile);

    // Asynchronously copy the data to the file.
    let istream = Cc["@mozilla.org/io/string-input-stream;1"].createInstance(
      Ci.nsIStringInputStream
    );
    istream.setUTF8Data(JSON.stringify(data));

    NetUtil.asyncCopy(istream, ostream, rc => {
      let success = Components.isSuccessCode(rc);
      if (!success) {
        debug("Failed to write configuration into " + path);
      }
      FileUtils.closeSafeFileOutputStream(ostream);
      if (callback) {
        callback(success);
      }
    });
  }

  return wifiConfigStore;
})();
