/* -*- Mode: IDL; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

[Exposed=ServiceWorker]
interface WebActivityRequestHandler {
  [Throws]
  undefined postResult(any result);

  undefined postError(DOMString error);

  [Pure, Cached, Frozen, Throws]
  readonly attribute WebActivityOptions source;

  /**
   * Sometimes we need to keep service worker alive to wait for data for
   * handler to post back.
   * In such case, using `event.waitUntil(handler.postDone());` can keep the
   * service worker alive until postDone is resolved, and postDone will be
   * resolved when postResult or postError is called.
   */
  Promise<undefined> postDone();
};
