/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BluetoothReplyRunnable.h"
#include "BluetoothService.h"
#include "BluetoothUtils.h"
#include "mozilla/dom/bluetooth/BluetoothCommon.h"
#include "mozilla/dom/bluetooth/BluetoothGatt.h"
#include "mozilla/dom/bluetooth/BluetoothTypes.h"
#include "mozilla/dom/BluetoothGattBinding.h"
#include "mozilla/dom/BluetoothGattCharacteristicEvent.h"
#include "mozilla/dom/Event.h"
#include "mozilla/dom/Promise.h"
#include "nsServiceManagerUtils.h"

using namespace mozilla;
using namespace mozilla::dom;

USING_BLUETOOTH_NAMESPACE

NS_IMPL_CYCLE_COLLECTION_CLASS(BluetoothGatt)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(BluetoothGatt,
                                                DOMEventTargetHelper)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mServices)

  /**
   * Unregister the bluetooth signal handler after unlinked.
   *
   * This is needed to avoid ending up with exposing a deleted object to JS or
   * accessing deleted objects while receiving signals from parent process
   * after unlinked. Please see Bug 1138267 for detail informations.
   */
  UnregisterBluetoothSignalHandler(tmp->mAppUuid, tmp);
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(BluetoothGatt,
                                                  DOMEventTargetHelper)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mServices)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(BluetoothGatt)
NS_INTERFACE_MAP_END_INHERITING(DOMEventTargetHelper)

NS_IMPL_ADDREF_INHERITED(BluetoothGatt, DOMEventTargetHelper)
NS_IMPL_RELEASE_INHERITED(BluetoothGatt, DOMEventTargetHelper)

BluetoothGatt::BluetoothGatt(nsPIDOMWindowInner* aWindow,
                             const nsAString& aDeviceAddr)
    : DOMEventTargetHelper(aWindow),
      mClientIf(0),
      mConnectionState(BluetoothConnectionState::Disconnected),
      mDeviceAddr(aDeviceAddr),
      mDiscoveringServices(false) {
  MOZ_ASSERT(aWindow);
  MOZ_ASSERT(!mDeviceAddr.IsEmpty());
}

BluetoothGatt::~BluetoothGatt() {
  BluetoothService* bs = BluetoothService::Get();
  // bs can be null on shutdown, where destruction might happen.
  NS_ENSURE_TRUE_VOID(bs);

  if (mClientIf > 0) {
    RefPtr<BluetoothVoidReplyRunnable> result =
        new BluetoothVoidReplyRunnable(nullptr);
    bs->UnregisterGattClientInternal(mClientIf, result);
  }

  UnregisterBluetoothSignalHandler(mAppUuid, this);
}

void BluetoothGatt::DisconnectFromOwner() {
  DOMEventTargetHelper::DisconnectFromOwner();

  BluetoothService* bs = BluetoothService::Get();
  NS_ENSURE_TRUE_VOID(bs);

  if (mClientIf > 0) {
    RefPtr<BluetoothVoidReplyRunnable> result =
        new BluetoothVoidReplyRunnable(nullptr);
    bs->UnregisterGattClientInternal(mClientIf, result);
  }

  UnregisterBluetoothSignalHandler(mAppUuid, this);
}

already_AddRefed<Promise> BluetoothGatt::Connect(ErrorResult& aRv) {
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  if (!global) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  RefPtr<Promise> promise = Promise::Create(global, aRv);
  NS_ENSURE_TRUE(!aRv.Failed(), nullptr);

  BT_ENSURE_TRUE_REJECT(
      mConnectionState == BluetoothConnectionState::Disconnected, promise,
      NS_ERROR_DOM_INVALID_STATE_ERR);

  BluetoothService* bs = BluetoothService::Get();
  BT_ENSURE_TRUE_REJECT(bs, promise, NS_ERROR_NOT_AVAILABLE);

  BluetoothAddress deviceAddr;
  BT_ENSURE_TRUE_REJECT(NS_SUCCEEDED(StringToAddress(mDeviceAddr, deviceAddr)),
                        promise, NS_ERROR_DOM_OPERATION_ERR);

  if (mAppUuid.IsEmpty()) {
    nsresult rv = GenerateUuid(mAppUuid);
    BT_ENSURE_TRUE_REJECT(NS_SUCCEEDED(rv) && !mAppUuid.IsEmpty(), promise,
                          NS_ERROR_DOM_OPERATION_ERR);
    RegisterBluetoothSignalHandler(mAppUuid, this);
  }

  BluetoothUuid appUuid;
  BT_ENSURE_TRUE_REJECT(NS_SUCCEEDED(StringToUuid(mAppUuid, appUuid)), promise,
                        NS_ERROR_DOM_OPERATION_ERR);

  UpdateConnectionState(BluetoothConnectionState::Connecting);
  bs->ConnectGattClientInternal(
      appUuid, deviceAddr, new BluetoothVoidReplyRunnable(nullptr, promise));

  return promise.forget();
}

already_AddRefed<Promise> BluetoothGatt::Disconnect(ErrorResult& aRv) {
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  if (!global) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  RefPtr<Promise> promise = Promise::Create(global, aRv);
  NS_ENSURE_TRUE(!aRv.Failed(), nullptr);

  BT_ENSURE_TRUE_REJECT(mConnectionState == BluetoothConnectionState::Connected,
                        promise, NS_ERROR_DOM_INVALID_STATE_ERR);

  BluetoothService* bs = BluetoothService::Get();
  BT_ENSURE_TRUE_REJECT(bs, promise, NS_ERROR_NOT_AVAILABLE);

  BluetoothUuid appUuid;
  BT_ENSURE_TRUE_REJECT(NS_SUCCEEDED(StringToUuid(mAppUuid, appUuid)), promise,
                        NS_ERROR_DOM_OPERATION_ERR);

  BluetoothAddress deviceAddr;
  BT_ENSURE_TRUE_REJECT(NS_SUCCEEDED(StringToAddress(mDeviceAddr, deviceAddr)),
                        promise, NS_ERROR_DOM_OPERATION_ERR);

  UpdateConnectionState(BluetoothConnectionState::Disconnecting);
  bs->DisconnectGattClientInternal(
      appUuid, deviceAddr, new BluetoothVoidReplyRunnable(nullptr, promise));

  return promise.forget();
}

class ReadRemoteRssiTask final : public BluetoothReplyRunnable {
 public:
  explicit ReadRemoteRssiTask(Promise* aPromise)
      : BluetoothReplyRunnable(nullptr, aPromise) {
    MOZ_ASSERT(aPromise);
  }

  bool ParseSuccessfulReply(JS::MutableHandle<JS::Value> aValue) {
    aValue.setUndefined();

    const BluetoothValue& v = mReply->get_BluetoothReplySuccess().value();
    NS_ENSURE_TRUE(v.type() == BluetoothValue::Tint32_t, false);

    aValue.setInt32(static_cast<int32_t>(v.get_int32_t()));
    return true;
  }
};

already_AddRefed<Promise> BluetoothGatt::ReadRemoteRssi(ErrorResult& aRv) {
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  if (!global) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  RefPtr<Promise> promise = Promise::Create(global, aRv);
  NS_ENSURE_TRUE(!aRv.Failed(), nullptr);

  BT_ENSURE_TRUE_REJECT(mConnectionState == BluetoothConnectionState::Connected,
                        promise, NS_ERROR_DOM_INVALID_STATE_ERR);

  BluetoothService* bs = BluetoothService::Get();
  BT_ENSURE_TRUE_REJECT(bs, promise, NS_ERROR_NOT_AVAILABLE);

  BluetoothAddress deviceAddr;
  BT_ENSURE_TRUE_REJECT(NS_SUCCEEDED(StringToAddress(mDeviceAddr, deviceAddr)),
                        promise, NS_ERROR_DOM_OPERATION_ERR);

  bs->GattClientReadRemoteRssiInternal(mClientIf, deviceAddr,
                                       new ReadRemoteRssiTask(promise));

  return promise.forget();
}

already_AddRefed<Promise> BluetoothGatt::DiscoverServices(ErrorResult& aRv) {
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  if (!global) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  RefPtr<Promise> promise = Promise::Create(global, aRv);
  NS_ENSURE_TRUE(!aRv.Failed(), nullptr);

  BluetoothUuid appUuid;
  BT_ENSURE_TRUE_REJECT(NS_SUCCEEDED(StringToUuid(mAppUuid, appUuid)), promise,
                        NS_ERROR_DOM_OPERATION_ERR);

  BT_ENSURE_TRUE_REJECT(
      mConnectionState == BluetoothConnectionState::Connected &&
          !mDiscoveringServices,
      promise, NS_ERROR_DOM_INVALID_STATE_ERR);

  BluetoothService* bs = BluetoothService::Get();
  BT_ENSURE_TRUE_REJECT(bs, promise, NS_ERROR_NOT_AVAILABLE);

  mDiscoveringServices = true;
  mServices.Clear();
  BluetoothGatt_Binding::ClearCachedServicesValue(this);

  bs->DiscoverGattServicesInternal(
      appUuid, new BluetoothVoidReplyRunnable(nullptr, promise));

  return promise.forget();
}

void BluetoothGatt::UpdateConnectionState(BluetoothConnectionState aState) {
  BT_LOGR("GATT connection state changes to: %d", int(aState));
  mConnectionState = aState;

  // Dispatch connectionstatechanged event to application
  RefPtr<Event> event = NS_NewDOMEvent(this, nullptr, nullptr);

  event->InitEvent(GATT_CONNECTION_STATE_CHANGED_ID, false, false);

  DispatchTrustedEvent(event);
}

void BluetoothGatt::HandleServicesDiscovered(const BluetoothValue& aValue) {
  const nsTArray<BluetoothGattDbElement>& dbElements =
      aValue.get_ArrayOfBluetoothGattDbElement();

  mServices.Clear();
  BluetoothGattService* currService = nullptr;
  BluetoothGattCharacteristic* currCharacteristic = nullptr;

  for (uint32_t i = 0; i < dbElements.Length(); i++) {
    switch (dbElements[i].mType) {
      case GATT_DB_TYPE_PRIMARY_SERVICE:
      case GATT_DB_TYPE_SECONDARY_SERVICE:
        currService = new BluetoothGattService(GetParentObject(), mAppUuid,
                                               dbElements[i]);
        mServices.AppendElement(currService);
        break;
      case GATT_DB_TYPE_CHARACTERISTIC:
        currCharacteristic = new BluetoothGattCharacteristic(
            GetParentObject(), currService, dbElements[i]);
        currService->AppendCharacteristic(currCharacteristic);
        break;
      case GATT_DB_TYPE_DESCRIPTOR:
        currCharacteristic->AppendDescriptor(new BluetoothGattDescriptor(
            GetParentObject(), currCharacteristic, dbElements[i]));
        break;
      case GATT_DB_TYPE_INCLUDED_SERVICE:
        currService->AppendIncludedService(new BluetoothGattService(
            GetParentObject(), mAppUuid, dbElements[i]));
        break;
      case GATT_DB_TYPE_END_GUARD:
      default:
        BT_WARNING("Unhandled GATT DB type: %d", dbElements[i].mType);
    }
  }

  BluetoothGatt_Binding::ClearCachedServicesValue(this);
}

void BluetoothGatt::HandleCharacteristicChanged(const BluetoothValue& aValue) {
  MOZ_ASSERT(aValue.type() == BluetoothValue::TArrayOfBluetoothNamedValue);

  const nsTArray<BluetoothNamedValue>& ids =
      aValue.get_ArrayOfBluetoothNamedValue();
  MOZ_ASSERT(ids.Length() == 1);  // Handle
  MOZ_ASSERT(ids[0].name().EqualsLiteral("gattHandle"));
  MOZ_ASSERT(ids[0].value().type() ==
             BluetoothValue::TBluetoothAttributeHandle);

  RefPtr<BluetoothGattCharacteristic> characteristic;
  for (uint32_t i = 0; i < mServices.Length(); i++) {
    nsTArray<RefPtr<BluetoothGattCharacteristic>> chars;
    mServices[i]->GetCharacteristics(chars);

    size_t index = chars.IndexOf(ids[0].value().get_BluetoothAttributeHandle());
    if (index != chars.NoIndex) {
      characteristic = chars.ElementAt(index);
    }
  }
  NS_ENSURE_TRUE_VOID(characteristic);

  // Dispatch characteristicchanged event to application
  BluetoothGattCharacteristicEventInit init;
  init.mCharacteristic = characteristic;
  RefPtr<BluetoothGattCharacteristicEvent> event =
      BluetoothGattCharacteristicEvent::Constructor(
          this, GATT_CHARACTERISTIC_CHANGED_ID, init);

  DispatchTrustedEvent(event);
}

void BluetoothGatt::Notify(const BluetoothSignal& aData) {
  BT_LOGD("[D] %s", NS_ConvertUTF16toUTF8(aData.name()).get());
  NS_ENSURE_TRUE_VOID(mSignalRegistered);

  BluetoothValue v = aData.value();
  if (aData.name().EqualsLiteral("ClientRegistered")) {
    MOZ_ASSERT(v.type() == BluetoothValue::Tuint32_t);
    mClientIf = v.get_uint32_t();
  } else if (aData.name().EqualsLiteral("ClientUnregistered")) {
    mClientIf = 0;
  } else if (
      aData.name().EqualsLiteral(
          "connectionstatechanged" /* GATT_CONNECTION_STATE_CHANGED_ID */)) {
    MOZ_ASSERT(v.type() == BluetoothValue::Tbool);

    BluetoothConnectionState state =
        v.get_bool() ? BluetoothConnectionState::Connected
                     : BluetoothConnectionState::Disconnected;
    UpdateConnectionState(state);
  } else if (aData.name().EqualsLiteral("ServicesDiscovered")) {
    HandleServicesDiscovered(v);
  } else if (aData.name().EqualsLiteral("DiscoverCompleted")) {
    MOZ_ASSERT(v.type() == BluetoothValue::Tbool);

    bool isDiscoverSuccess = v.get_bool();
    if (!isDiscoverSuccess) {  // Clean all discovered attributes if failed
      mServices.Clear();
      BluetoothGatt_Binding::ClearCachedServicesValue(this);
    }

    mDiscoveringServices = false;
  } else if (aData.name().Equals(GATT_CHARACTERISTIC_CHANGED_ID)) {
    HandleCharacteristicChanged(v);
  } else {
    BT_WARNING("Not handling GATT signal: %s",
               NS_ConvertUTF16toUTF8(aData.name()).get());
  }
}

JSObject* BluetoothGatt::WrapObject(JSContext* aContext,
                                    JS::Handle<JSObject*> aGivenProto) {
  return BluetoothGatt_Binding::Wrap(aContext, this, aGivenProto);
}
