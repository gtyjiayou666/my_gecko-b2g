/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cellbroadcast/CellBroadcastParent.h"
#include "nsServiceManagerUtils.h"

namespace mozilla {
namespace dom {
namespace cellbroadcast {

NS_IMPL_ISUPPORTS(CellBroadcastParent, nsICellBroadcastListener)

mozilla::ipc::IPCResult CellBroadcastParent::Init() {
  nsCOMPtr<nsICellBroadcastService> service =
      do_GetService(CELLBROADCAST_SERVICE_CONTRACTID);

  if (service) {
    if (NS_SUCCEEDED(service->RegisterListener(this))) {
      return IPC_OK();
    } else {
      return IPC_FAIL_NO_REASON(this);
    }
  }

  return IPC_FAIL_NO_REASON(this);
}

void CellBroadcastParent::ActorDestroy(ActorDestroyReason aWhy) {
  nsCOMPtr<nsICellBroadcastService> service =
      do_GetService(CELLBROADCAST_SERVICE_CONTRACTID);
  if (service) {
    service->UnregisterListener(this);
  }
}

/*
 * nsICellBroadcastListener
 */
NS_IMETHODIMP
CellBroadcastParent::NotifyMessageReceived(
    uint32_t aServiceId, uint32_t aGsmGeographicalScope, uint16_t aMessageCode,
    uint16_t aMessageId, const nsAString& aLanguage, const nsAString& aBody,
    uint32_t aMessageClass, DOMTimeStamp aTimestamp,
    uint32_t aCdmaServiceCategory, bool aHasEtwsInfo, uint32_t aEtwsWarningType,
    bool aEtwsEmergencyUserAlert, bool aEtwsPopup, uint16_t aUpdateNumber) {
  return SendNotifyReceivedMessage(
             aServiceId, aGsmGeographicalScope, aMessageCode, aMessageId,
             nsString(aLanguage), nsString(aBody), aMessageClass, aTimestamp,
             aCdmaServiceCategory, aHasEtwsInfo, aEtwsWarningType,
             aEtwsEmergencyUserAlert, aEtwsPopup, aUpdateNumber)
             ? NS_OK
             : NS_ERROR_FAILURE;
}

}  // namespace cellbroadcast
}  // namespace dom
}  // namespace mozilla
