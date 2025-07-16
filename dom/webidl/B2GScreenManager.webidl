/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

dictionary Resolution {
  long width;
  long height;
};

dictionary Pos {
  long x;
  long y;
  long width;
  long height;
};

[Exposed=Window, Pref="dom.screen.enabled", Func="B2G::HasB2GScreenManagerSupport"]
interface B2GScreenManager {
    Promise<long> getScreenNum();
    Promise<Resolution> getCurrentResolution(long index);
    Promise<sequence<Resolution>> getScreenResolutions(long index);
    Promise<Pos> setResolution(long screen_num, long extension_mod, long new_width, long new_height);
};
