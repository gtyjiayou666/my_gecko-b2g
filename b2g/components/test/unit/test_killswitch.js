/* eslint-disable no-undef */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

// Trivial test just to make sure we have no syntax error
add_test(function test_ksm_ok() {
  ok(KillSwitchMain, "KillSwitchMain object exists");

  run_next_test();
});

var aMessageNoPerm = {
  name: "KillSwitch:Enable",
  target: {
    assertPermission() {
      return false;
    },
    killChild() {},
  },
};

var aMessageWithPerm = {
  name: "KillSwitch:Enable",
  target: {
    assertPermission() {
      return true;
    },
  },
  data: {
    requestID: 0,
  },
};

add_test(function test_sendMessageWithoutPerm() {
  try {
    KillSwitchMain.receiveMessage(aMessageNoPerm);
    ok(false, "Should have failed");
  } catch (ex) {
    // strictEqual(ex, Cr.NS_ERROR_NOT_AVAILABLE);
  }
  run_next_test();
});

add_test(function test_sendMessageWithPerm() {
  let rv = KillSwitchMain.receiveMessage(aMessageWithPerm);
  strictEqual(rv, undefined);
  run_next_test();
});

var uMessage = {
  name: "KillSwitch:WTF",
  target: {
    assertPermission() {
      return true;
    },
    killChild() {},
  },
};

add_test(function test_sendUnknownMessage() {
  try {
    KillSwitchMain.receiveMessage(uMessage);
    ok(false, "Should have failed");
  } catch (ex) {
    strictEqual(ex, Cr.NS_ERROR_ILLEGAL_VALUE);
  }
  run_next_test();
});

var fakeLibcUtils = {
  _props_: {},
  property_set(name, value) {
    dump(
      "property_set('" + name + "', '" + value + "' [" + typeof value + "]);\n"
    );
    this._props_[name] = value;
  },
  property_get(name, defaultValue) {
    dump("property_get('" + name + "', '" + defaultValue + "');\n");
    if (Object.keys(this._props_).includes(name)) {
      return this._props_[name];
    }
    return defaultValue;
  },
};

add_test(function test_nolibcutils() {
  KillSwitchMain._libcutils = null;
  try {
    KillSwitchMain.checkLibcUtils();
    ok(false, "Should have failed");
  } catch (ex) {
    strictEqual(ex, Cr.NS_ERROR_NO_INTERFACE);
  }
  run_next_test();
});

add_test(function test_install_fakelibcutils() {
  KillSwitchMain._libcutils = fakeLibcUtils;
  let rv = KillSwitchMain.checkLibcUtils();
  strictEqual(rv, true);
  run_next_test();
});

install_common_tests();
