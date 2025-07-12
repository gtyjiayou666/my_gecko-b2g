/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

enum TetheringType {
  "bluetooth",
  "usb",
  "wifi"
};

enum SecurityType {
  "open",
  "wpa-psk",
  "wpa2-psk"
};

dictionary TetheringConfiguration {
  DOMString ssid;
  SecurityType security;
  DOMString key;
  DOMString ip;
  DOMString prefix;
  DOMString startIp;
  DOMString endIp;
  DOMString dns1;
  DOMString dns2;
  unsigned long channel;
};

[JSImplementation="@mozilla.org/tetheringconfiginfo;1",
 Func="B2G::HasTetheringManagerSupport",
 Exposed=Window]
interface TetheringConfigInfo {
  constructor(optional TetheringConfiguration tetheringConfigurationDict = {});
  readonly attribute DOMString? ssid;
  readonly attribute SecurityType? security;
  readonly attribute DOMString? key;
  readonly attribute DOMString? ip;
  readonly attribute DOMString? prefix;
  readonly attribute DOMString? startIp;
  readonly attribute DOMString? endIp;
  readonly attribute DOMString? dns1;
  readonly attribute DOMString? dns2;
  readonly attribute unsigned long channel;
};


[JSImplementation="@mozilla.org/tetheringmanager;1",
 Func="B2G::HasTetheringManagerSupport",
 Exposed=Window]
interface TetheringManager : EventTarget {

  /**
   * tetheringState sync from nsITetheringService.idl
   */

  const unsigned long TETHERING_STATE_INACTIVE = 0;

  const unsigned long TETHERING_STATE_ACTIVE   = 1;

  /**
   * Enable/Disable tethering.
   * @param enabled True to enable tethering, False to disable tethering.
   * @param type Tethering type to enable/disable.
   * @param config Configuration should have following fields when enable is True:
   *               - ssid SSID network name.
   *               - security open, wpa-psk or wpa2-psk.
   *               - key password for wpa-psk or wpa2-psk.
   *               - ip ip address.
   *               - prefix mask length.
   *               - startIp start ip address allocated by DHCP server for tethering.
   *               - endIp end ip address allocated by DHCP server for tethering.
   *               - dns1 first DNS server address.
   *               - dns2 second DNS server address.
   *               config should not be set when enabled is False.
   */
  Promise<any> setTetheringEnabled(boolean enabled,
                                   TetheringType type,
                                   optional TetheringConfiguration config = {});

  /**
   * Returns wifi tethering state. One of the TETHERING_STATE_* constants.
   */
  readonly attribute long wifiTetheringState;

  /**
   * Returns usb tethering state. One of the TETHERING_STATE_* constants.
   */
  readonly attribute long usbTetheringState;

  /**
   * An event listener that is called with notification about the tethering
   * connection status.
   */
  attribute EventHandler ontetheringstatuschange;

  /**
   * An event listener that is called with notification about the tethering
   * configuration.
   */
  attribute EventHandler ontetheringconfigchange;

  /**
   * Return wifi tethering current configuration.
   */
  readonly attribute TetheringConfigInfo wifiTetheringConfig;

  /**
   * Return usb tethering current configuration.
   */
  readonly attribute TetheringConfigInfo usbTetheringConfig;

};
