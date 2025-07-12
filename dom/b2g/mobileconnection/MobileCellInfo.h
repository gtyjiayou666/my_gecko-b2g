/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_MobileCellInfo_h
#define mozilla_dom_MobileCellInfo_h

#include "nsIMobileCellInfo.h"
#include "nsPIDOMWindow.h"
#include "nsWrapperCache.h"

namespace mozilla {
namespace dom {

class MobileCellInfo final : public nsIMobileCellInfo, public nsWrapperCache {
 public:
  NS_DECL_NSIMOBILECELLINFO
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(MobileCellInfo)

  explicit MobileCellInfo(nsPIDOMWindowInner* aWindow);

  MobileCellInfo(int32_t aGsmLocationAreaCode, int64_t aGsmCellId,
                 int32_t aCdmaBaseStationId, int32_t aCdmaBaseStationLatitude,
                 int32_t aCdmaBaseStationLongitude, int32_t aCdmaSystemId,
                 int32_t aCdmaNetworkId, int16_t aCdmaRoamingIndicator,
                 int16_t aCdmaDefaultRoamingIndicator, bool aCdmaSystemIsInPRL,
                 int32_t aTac, int64_t aCi, int32_t aPci, int32_t aArfcns,
                 int64_t aBands);

  void Update(nsIMobileCellInfo* aInfo);

  nsPIDOMWindowInner* GetParentObject() const { return mWindow; }

  virtual JSObject* WrapObject(JSContext* aCx,
                               JS::Handle<JSObject*> aGivenProto) override;

  // WebIDL interface
  int32_t GsmLocationAreaCode() const { return mGsmLocationAreaCode; }

  int64_t GsmCellId() const { return mGsmCellId; }

  int32_t CdmaBaseStationId() const { return mCdmaBaseStationId; }

  int32_t CdmaBaseStationLatitude() const { return mCdmaBaseStationLatitude; }

  int32_t CdmaBaseStationLongitude() const { return mCdmaBaseStationLongitude; }

  int32_t CdmaSystemId() const { return mCdmaSystemId; }

  int32_t CdmaNetworkId() const { return mCdmaNetworkId; }

  int16_t CdmaRoamingIndicator() const { return mCdmaRoamingIndicator; }

  int16_t CdmaDefaultRoamingIndicator() const {
    return mCdmaDefaultRoamingIndicator;
  }

  bool CdmaSystemIsInPRL() const { return mCdmaSystemIsInPRL; }

  int32_t Tac() const { return mTac; }
  int64_t Ci() const { return mCi; }
  int32_t Pci() const { return mPci; }
  int32_t Arfcns() const { return mArfcns; }
  int64_t Bands() const { return mBands; }

 private:
  ~MobileCellInfo() {}

 private:
  nsCOMPtr<nsPIDOMWindowInner> mWindow;
  int32_t mGsmLocationAreaCode;
  int64_t mGsmCellId;
  int32_t mCdmaBaseStationId;
  int32_t mCdmaBaseStationLatitude;
  int32_t mCdmaBaseStationLongitude;
  int32_t mCdmaSystemId;
  int32_t mCdmaNetworkId;
  int16_t mCdmaRoamingIndicator;
  int16_t mCdmaDefaultRoamingIndicator;
  bool mCdmaSystemIsInPRL;
  int32_t mTac;
  int64_t mCi;
  int32_t mPci;
  int32_t mArfcns;
  int64_t mBands;
};

}  // namespace dom
}  // namespace mozilla

#endif  // mozilla_dom_MobileCellInfo_h
