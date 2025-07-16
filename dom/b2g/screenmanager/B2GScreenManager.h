/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_screen_B2GScreenManager_h
#define mozilla_dom_screen_B2GScreenManager_h

#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/dom/B2GScreenManagerBinding.h"
#include "nsWrapperCache.h"

#include "nsThreadUtils.h"
class nsIGlobalObject;

namespace mozilla {

namespace dom {

class B2GScreenManager final : public nsISupports, public nsWrapperCache {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(B2GScreenManager)
  explicit B2GScreenManager(nsIGlobalObject* aGlobal);

  nsIGlobalObject* GetParentObject() const { return mGlobal; }
  // void Init();

  virtual JSObject* WrapObject(JSContext* aCx,
                               JS::Handle<JSObject*> aGivenProto) override;

  already_AddRefed<Promise> GetScreenNum();
  already_AddRefed<Promise> GetCurrentResolution(int32_t index);
  already_AddRefed<Promise> GetScreenResolutions(int32_t index);
  already_AddRefed<Promise> SetResolution(int32_t screen_num,
                                          int32_t extension_mod,
                                          int32_t new_width,
                                          int32_t new_height);

 protected:
  ~B2GScreenManager() = default;
  nsCOMPtr<nsIGlobalObject> mGlobal;

};

}  // namespace dom
}  // namespace mozilla

#endif  // mozilla_dom_screen_B2GScreenManager_h
