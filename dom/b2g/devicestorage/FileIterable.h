/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_FileIterable_h
#define mozilla_dom_FileIterable_h

#include "nsCOMPtr.h"
#include "nsWrapperCache.h"
#include "mozilla/dom/IterableIterator.h"

class nsIGlobalObject;
class DeviceStorageCursorRequest;

namespace mozilla {

class ErrorResult;
class File;

namespace dom {

class FileIterable final : public nsISupports, public nsWrapperCache {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(FileIterable)

  explicit FileIterable(nsIGlobalObject* aGlobal,
                        DeviceStorageCursorRequest* aRequest);
  nsIGlobalObject* GetParentObject() const;
  virtual JSObject* WrapObject(JSContext* aCx,
                               JS::Handle<JSObject*> aGivenProto) override;

  struct IteratorData {
    nsTArray<RefPtr<Promise>> mPromises;
  };

  using Iterator = AsyncIterableIterator<FileIterable>;

  void InitAsyncIteratorData(IteratorData& aData, Iterator::IteratorType aType,
                             ErrorResult& aError) {}

  already_AddRefed<Promise> GetNextIterationResult(Iterator* aIterator,
                                                   ErrorResult& aRv);

  void FireSuccess(JS::Handle<JS::Value> aResult);
  void FireError(const nsString& aReason);
  void FireDone();
  void EnumeratePrepared();

 private:
  ~FileIterable();
  void RejectWithReason(Promise* aPromise, const nsString& aReason);

  enum EnumerateState { Initialized = 0, Prepared, Looping, Abort, Done };

  RefPtr<Iterator> mIterator;
  nsCOMPtr<nsIGlobalObject> mGlobal;
  RefPtr<DeviceStorageCursorRequest> mRequest;
  EnumerateState mState;
  uint32_t mPendingContinue;
  nsString mRejectReason;
};

}  // namespace dom
}  // namespace mozilla

#endif  // mozilla_dom_FileIterable_h
