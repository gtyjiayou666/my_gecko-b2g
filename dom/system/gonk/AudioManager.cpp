/* Copyright 2012 Mozilla Foundation and Mozilla contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "AudioManager.h"
#include "AudioChannelService.h"
#include "BluetoothCommon.h"
#include "BluetoothHfpManagerBase.h"
#include "base/message_loop.h"
#include "GonkAudioSystem.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Hal.h"
#include "mozilla/ModuleUtils.h"
#include "mozilla/MozPromise.h"
#include "mozilla/Preferences.h"
#include "mozilla/Services.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/dom/ToJSValue.h"
#include "mozilla/dom/power/PowerManagerService.h"
#include "nsComponentManagerUtils.h"
#include "nsContentUtils.h"
#include "nsIObserverService.h"
#include "nsISettings.h"
#include "nsJSUtils.h"
#include "nsPrintfCString.h"
#include "nsServiceManagerUtils.h"
#include "nsThreadUtils.h"
#include "nsXULAppAPI.h"

#include <android/log.h>
#include <binder/IServiceManager.h>
#include <cutils/properties.h>

#ifdef MOZ_B2G_RIL
#  include "nsIRadioInterfaceLayer.h"
#  include "nsITelephonyService.h"
#endif

using android::DeviceTypeSet;
using android::GonkAudioSystem;
using android::status_t;
using android::String16;
using android::String8;
using mozilla::dom::AudioChannel;
using mozilla::dom::bluetooth::BluetoothHfpManagerBase;
using mozilla::dom::bluetooth::BluetoothProfileManagerBase;

mozilla::LazyLogModule gAudioManagerLog("AudioManager");
#undef LOG
#undef LOGE

#define LOG(fmt, ...) \
  MOZ_LOG(gAudioManagerLog, mozilla::LogLevel::Debug, (fmt, ##__VA_ARGS__))
#define LOGE(fmt, ...) \
  MOZ_LOG(gAudioManagerLog, mozilla::LogLevel::Error, (fmt, ##__VA_ARGS__))

#define BLUETOOTH_HFP_STATUS_CHANGED "bluetooth-hfp-status-changed"
#define BLUETOOTH_SCO_STATUS_CHANGED "bluetooth-sco-status-changed"
#define HEADPHONES_STATUS_HEADSET u"headset"
#define HEADPHONES_STATUS_HEADPHONE u"headphone"
#define HEADPHONES_STATUS_LINEOUT u"lineout"
#define HEADPHONES_STATUS_OFF u"off"
#define HEADPHONES_STATUS_UNKNOWN u"unknown"
#define HEADPHONES_STATUS_CHANGED "headphones-status-changed"
#define SCREEN_STATE_CHANGED "screen-state-changed"
#define AUDIO_POLICY_SERVICE_NAME "media.audio_policy"
#define SETTINGS_MANAGER "@mozilla.org/sidl-native/settings;1"

namespace mozilla {
namespace dom {
namespace gonk {

// Refer AudioService.java from Android
static const uint32_t sMaxStreamVolumeTbl[AUDIO_STREAM_PUBLIC_CNT] = {
    5,   // voice call
    15,  // system
    15,  // ring
    15,  // music
    15,  // alarm
    15,  // notification
    15,  // BT SCO
    15,  // enforced audible
    15,  // DTMF
    15,  // TTS
    15,  // accessibility
};

static const uint32_t sMinStreamVolumeTbl[AUDIO_STREAM_PUBLIC_CNT] = {
    1,  // voice call
    0,  // system
    0,  // ring
    0,  // music
    1,  // alarm
    0,  // notification
    0,  // BT SCO
    0,  // enforced audible
    0,  // DTMF
    0,  // TTS
    1,  // accessibility
};

static const uint32_t sDefaultStreamVolumeTbl[AUDIO_STREAM_PUBLIC_CNT] = {
    3,   // voice call
    8,   // system
    8,   // ring
    8,   // music
    8,   // alarm
    8,   // notification
    8,   // BT SCO
    15,  // enforced audible  // XXX Handle as fixed maximum audio setting
    8,   // DTMF
    8,   // TTS
    8,   // accessibility
};

static const audio_stream_type_t
    sStreamVolumeAliasTbl[AUDIO_STREAM_PUBLIC_CNT] = {
        AUDIO_STREAM_VOICE_CALL,        // voice call
        AUDIO_STREAM_NOTIFICATION,      // system
        AUDIO_STREAM_NOTIFICATION,      // ring
        AUDIO_STREAM_MUSIC,             // music
        AUDIO_STREAM_ALARM,             // alarm
        AUDIO_STREAM_NOTIFICATION,      // notification
        AUDIO_STREAM_BLUETOOTH_SCO,     // BT SCO
        AUDIO_STREAM_ENFORCED_AUDIBLE,  // enforced audible
        AUDIO_STREAM_DTMF,              // DTMF
        AUDIO_STREAM_TTS,               // TTS
        AUDIO_STREAM_ACCESSIBILITY,     // accessibility
};

static const audio_stream_type_t sChannelStreamTbl[NUMBER_OF_AUDIO_CHANNELS] = {
    AUDIO_STREAM_MUSIC,             // AudioChannel::Normal
    AUDIO_STREAM_MUSIC,             // AudioChannel::Content
    AUDIO_STREAM_NOTIFICATION,      // AudioChannel::Notification
    AUDIO_STREAM_ALARM,             // AudioChannel::Alarm
    AUDIO_STREAM_VOICE_CALL,        // AudioChannel::Telephony
    AUDIO_STREAM_RING,              // AudioChannel::Ringer
    AUDIO_STREAM_ENFORCED_AUDIBLE,  // AudioChannel::Publicnotification
    AUDIO_STREAM_SYSTEM,            // AudioChannel::System
};

struct AudioDeviceInfo {
  /** The string the value maps to */
  nsLiteralString tag;
  /** The enum value that maps to this string */
  audio_devices_t value;
};

// Mappings audio output devices to strings.
static const AudioDeviceInfo kAudioDeviceInfos[] = {
    {u"earpiece"_ns, AUDIO_DEVICE_OUT_EARPIECE},
    {u"speaker"_ns, AUDIO_DEVICE_OUT_SPEAKER},
    {u"wired_headset"_ns, AUDIO_DEVICE_OUT_WIRED_HEADSET},
    {u"wired_headphone"_ns, AUDIO_DEVICE_OUT_WIRED_HEADPHONE},
    {u"bt_scoheadset"_ns, AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET},
    {u"bt_a2dp"_ns, AUDIO_DEVICE_OUT_BLUETOOTH_A2DP},
};

static const int kBtSampleRate = 8000;
static const int kBtWideBandSampleRate = 16000;

static inline nsresult ToNsresult(status_t aErr) {
  return aErr == android::OK ? NS_OK : NS_ERROR_FAILURE;
}

static inline String8 ToString8(const nsACString& aStr) {
  return String8(aStr.Data(), aStr.Length());
}

/**
 * We have five sound volume settings from UX spec,
 * You can see more informations in Bug1068219.
 * (1) Media : music, video, FM ...
 * (2) Notification : ringer, notification ...
 * (3) Alarm : alarm
 * (4) Telephony : GSM call, WebRTC call
 * (5) Bluetooth SCO : SCO call
 **/
struct VolumeData {
  nsLiteralString mChannelName;
  audio_stream_type_t mStreamType;
};

static const VolumeData gVolumeData[] = {
    {u"audio.volume.content"_ns, AUDIO_STREAM_MUSIC},
    {u"audio.volume.notification"_ns, AUDIO_STREAM_NOTIFICATION},
    {u"audio.volume.alarm"_ns, AUDIO_STREAM_ALARM},
    {u"audio.volume.telephony"_ns, AUDIO_STREAM_VOICE_CALL},
    {u"audio.volume.bt_sco"_ns, AUDIO_STREAM_BLUETOOTH_SCO}};

class GonkAudioPortCallback : public GonkAudioSystem::AudioPortCallback {
 public:
  virtual void onAudioPortListUpdate() {
    nsCOMPtr<nsIRunnable> runnable = NS_NewRunnableFunction(
        "GonkAudioPortCallback::onAudioPortListUpdate", []() {
          MOZ_ASSERT(NS_IsMainThread());
          RefPtr<AudioManager> audioManager = AudioManager::GetInstance();
          NS_ENSURE_TRUE(audioManager.get(), );
          audioManager->MaybeWriteVolumeSettings();
        });
    NS_DispatchToMainThread(runnable);
  }
  virtual void onAudioPatchListUpdate() {}
  virtual void onServiceDied() {}
};

// We need to store GonkAudioPortCallback instance in AudioManager class, but
// we don't want to expose android::sp in the header, so add a holder to hide
// it. If there are more similar cases in the future, we need a private
// AudioManager class instead.
class AudioPortCallbackHolder {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(AudioPortCallbackHolder);

  android::sp<GonkAudioSystem::AudioPortCallback> Callback() {
    if (!mCallback) {
      mCallback = new GonkAudioPortCallback();
    }
    return mCallback;
  }

 private:
  ~AudioPortCallbackHolder(){};
  android::sp<GonkAudioSystem::AudioPortCallback> mCallback;
};

void AudioManager::HandleAudioFlingerDied() {
  // Disable volume change notification
  mIsVolumeInited = false;

  uint32_t attempt;
  for (attempt = 0; attempt < 50; attempt++) {
    if (android::defaultServiceManager()->checkService(
            String16(AUDIO_POLICY_SERVICE_NAME)) != 0) {
      break;
    }
    LOG("AudioPolicyService is dead! attempt=%d", attempt);
    usleep(1000 * 200);
  }

  MOZ_RELEASE_ASSERT(attempt < 50);

  // Indicate to audio HAL that we start the reconfiguration phase after a media
  // server crash
  SetParameters("restarting=true");

  // Restore device connection states
  SetAllDeviceConnectionStates();

  // Restore call state
  GonkAudioSystem::setPhoneState(mPhoneState);

  // Restore master volume/mono/balance
  GonkAudioSystem::setMasterVolume(1.0);
  GonkAudioSystem::setMasterMono(mMasterMono);
  GonkAudioSystem::setMasterBalance(mMasterBalance);

  GonkAudioSystem::setAssistantUid(AUDIO_UID_INVALID);
  GonkAudioSystem::setForceUse(AUDIO_POLICY_FORCE_FOR_SYSTEM,
                               AUDIO_POLICY_FORCE_SYSTEM_ENFORCED);

  // Restore stream volumes
  for (auto& streamState : mStreamStates) {
    streamState->InitStreamVolume();
    streamState->RestoreVolumeIndexToAllDevices();
  }

  // Enable volume change notification
  mIsVolumeInited = true;
  MaybeWriteVolumeSettings(true);

  // Indicate the end of reconfiguration phase to audio HAL
  SetParameters("restarting=false");
}

class SettingInfo final : public nsISettingInfo {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSISETTINGINFO

  explicit SettingInfo(const nsAString& aName, nsAString& aValue);

 protected:
  ~SettingInfo() = default;

 private:
  nsString mName;
  nsString mValue;
};

NS_IMPL_ISUPPORTS(SettingInfo, nsISettingInfo)
SettingInfo::SettingInfo(const nsAString& aName, nsAString& aValue)
    : mName(aName), mValue(aValue) {}

NS_IMETHODIMP
SettingInfo::GetName(nsAString& aName) {
  aName = mName;
  return NS_OK;
}

NS_IMETHODIMP
SettingInfo::SetName(const nsAString& aName) {
  mName = aName;
  return NS_OK;
}

NS_IMETHODIMP
SettingInfo::GetValue(nsAString& aValue) {
  aValue = mValue;
  return NS_OK;
}

NS_IMETHODIMP
SettingInfo::SetValue(const nsAString& aValue) {
  mValue = aValue;
  return NS_OK;
}

class AudioSettingsGetCallback final : public nsISettingsGetBatchResponse {
 public:
  NS_DECL_ISUPPORTS

  NS_IMETHOD Resolve(
      const nsTArray<RefPtr<nsISettingInfo>>& aSettings) override {
    RefPtr<AudioManager> audioManager = AudioManager::GetInstance();
    for (auto& setting : aSettings) {
      nsString name, value;
      setting->GetName(name);
      setting->GetValue(value);
      audioManager->OnAudioSettingChanged(name, value);
    }
    audioManager->ReadAudioSettingsFinished();
    return NS_OK;
  }

  NS_IMETHOD Reject(nsISettingError* aError) override {
    nsString name;
    uint16_t reason;
    aError->GetName(name);
    aError->GetReason(&reason);
    LOGE("AudioSettingsGetCallback::Reject, name: %s, reason: %s",
         NS_ConvertUTF16toUTF8(name).get(),
         (reason == nsISettingError::NON_EXISTING_SETTING)
             ? "NON_EXISTING_SETTING"
             : "UNKNOWN_ERROR");

    RefPtr<AudioManager> audioManager = AudioManager::GetInstance();
    audioManager->ReadAudioSettingsFinished();
    return NS_OK;
  }

 protected:
  ~AudioSettingsGetCallback() = default;
};

NS_IMPL_ISUPPORTS(AudioSettingsGetCallback, nsISettingsGetBatchResponse)

class AudioSettingsObserver final : public nsISettingsObserver {
 public:
  NS_DECL_ISUPPORTS

  NS_IMETHOD ObserveSetting(nsISettingInfo* aSetting) override {
    nsString name, value;
    aSetting->GetName(name);
    aSetting->GetValue(value);
    RefPtr<AudioManager> audioManager = AudioManager::GetInstance();
    audioManager->OnAudioSettingChanged(name, value);
    return NS_OK;
  }

 protected:
  ~AudioSettingsObserver() = default;
};

NS_IMPL_ISUPPORTS(AudioSettingsObserver, nsISettingsObserver)

class GenericSidlCallback final : public nsISidlDefaultResponse {
 public:
  NS_DECL_ISUPPORTS

  NS_IMETHODIMP Resolve() override { return NS_OK; }

  NS_IMETHODIMP Reject() override {
    LOGE("GenericSidlCallback::Rejet, call site %s", mCallSite.get());
    return NS_OK;
  }

  explicit GenericSidlCallback(const char* aCallSite) : mCallSite(aCallSite) {}

 protected:
  ~GenericSidlCallback() = default;

 private:
  nsCString mCallSite;
};

NS_IMPL_ISUPPORTS(GenericSidlCallback, nsISidlDefaultResponse)

static void BinderDeadCallback(status_t aErr) {
  if (aErr != android::DEAD_OBJECT) {
    return;
  }

  nsCOMPtr<nsIRunnable> runnable =
      NS_NewRunnableFunction("AudioManager::BinderDeadCallback", []() {
        MOZ_ASSERT(NS_IsMainThread());
        RefPtr<AudioManager> audioManager = AudioManager::GetInstance();
        NS_ENSURE_TRUE(audioManager.get(), );
        audioManager->HandleAudioFlingerDied();
      });

  NS_DispatchToMainThread(runnable);
}

bool AudioManager::IsFmOutConnected() {
#if defined(PRODUCT_MANUFACTURER_QUALCOMM)
  return GetParameters("fm_status") == "fm_status=1"_ns;
#elif defined(PRODUCT_MANUFACTURER_SPRD)
  return mConnectedDevices.count(AUDIO_DEVICE_OUT_FM_HEADSET) ||
         mConnectedDevices.count(AUDIO_DEVICE_OUT_FM_SPEAKER);
#elif defined(PRODUCT_MANUFACTURER_MTK)
  return mConnectedDevices.count(AUDIO_DEVICE_IN_FM_TUNER);
#else
  // MOZ_CRASH("FM radio not supported");
  return false;
#endif
}

NS_IMPL_ISUPPORTS(AudioManager, nsIAudioManager, nsIObserver)

void AudioManager::UpdateHeadsetConnectionState(hal::SwitchState aState) {
  bool headphoneConnected =
      mConnectedDevices.count(AUDIO_DEVICE_OUT_WIRED_HEADPHONE);
  bool headsetConnected =
      mConnectedDevices.count(AUDIO_DEVICE_OUT_WIRED_HEADSET);
  bool lineoutConnected = mConnectedDevices.count(AUDIO_DEVICE_OUT_LINE);

  if (aState == hal::SWITCH_STATE_HEADSET) {
    UpdateDeviceConnectionState(true, AUDIO_DEVICE_OUT_WIRED_HEADSET);
    UpdateDeviceConnectionState(true, AUDIO_DEVICE_IN_WIRED_HEADSET);
  } else if (aState == hal::SWITCH_STATE_HEADPHONE) {
    UpdateDeviceConnectionState(true, AUDIO_DEVICE_OUT_WIRED_HEADPHONE);
  } else if (aState == hal::SWITCH_STATE_LINEOUT) {
    UpdateDeviceConnectionState(true, AUDIO_DEVICE_OUT_LINE);
  } else if (aState == hal::SWITCH_STATE_OFF) {
    if (headsetConnected) {
      UpdateDeviceConnectionState(false, AUDIO_DEVICE_OUT_WIRED_HEADSET);
      UpdateDeviceConnectionState(false, AUDIO_DEVICE_IN_WIRED_HEADSET);
    }
    if (headphoneConnected) {
      UpdateDeviceConnectionState(false, AUDIO_DEVICE_OUT_WIRED_HEADPHONE);
    }
    if (lineoutConnected) {
      UpdateDeviceConnectionState(false, AUDIO_DEVICE_OUT_LINE);
    }
  }
}

void AudioManager::UpdateDeviceConnectionState(
    bool aIsConnected, audio_devices_t aDevice,
    const nsCString& aDeviceAddress) {
  // If the connection state is not changed, just return.
  if (aIsConnected == bool(mConnectedDevices.count(aDevice))) {
    return;
  }

  if (aIsConnected) {
    mConnectedDevices[aDevice] = aDeviceAddress;
  } else {
    mConnectedDevices.erase(aDevice);
  }
  GonkAudioSystem::setDeviceConnected(aDevice, aIsConnected,
                                      ToString8(aDeviceAddress));
}

void AudioManager::SetAllDeviceConnectionStates() {
  for (const auto& [device, deviceAddress] : mConnectedDevices) {
    GonkAudioSystem::setDeviceConnected(device, true, ToString8(deviceAddress));
  }
}

void AudioManager::HandleBluetoothStatusChanged(nsISupports* aSubject,
                                                const char* aTopic,
                                                const nsCString aAddress) {
#ifdef MOZ_B2G_BT
  bool isConnected = false;
  if (!strcmp(aTopic, BLUETOOTH_SCO_STATUS_CHANGED)) {
    auto* hfp = static_cast<BluetoothHfpManagerBase*>(aSubject);
    isConnected = hfp->IsScoConnected();
  } else {
    auto* profile = static_cast<BluetoothProfileManagerBase*>(aSubject);
    isConnected = profile->IsConnected();
  }

  if (!strcmp(aTopic, BLUETOOTH_SCO_STATUS_CHANGED)) {
    if (isConnected) {
      auto* hfp = static_cast<BluetoothHfpManagerBase*>(aSubject);
      int btSampleRate =
          hfp->IsWbsEnabled() ? kBtWideBandSampleRate : kBtSampleRate;
      SetParameters("bt_samplerate=%d", btSampleRate);
      SetParameters("BT_SCO=on");
      GonkAudioSystem::setForceUse(AUDIO_POLICY_FORCE_FOR_COMMUNICATION,
                                   AUDIO_POLICY_FORCE_BT_SCO);
      GonkAudioSystem::setForceUse(AUDIO_POLICY_FORCE_FOR_RECORD,
                                   AUDIO_POLICY_FORCE_BT_SCO);
    } else {
      SetParameters("BT_SCO=off");
      if (GonkAudioSystem::getForceUse(AUDIO_POLICY_FORCE_FOR_COMMUNICATION) ==
          AUDIO_POLICY_FORCE_BT_SCO) {
        GonkAudioSystem::setForceUse(AUDIO_POLICY_FORCE_FOR_COMMUNICATION,
                                     AUDIO_POLICY_FORCE_NONE);
      }
      if (GonkAudioSystem::getForceUse(AUDIO_POLICY_FORCE_FOR_RECORD) ==
          AUDIO_POLICY_FORCE_BT_SCO) {
        GonkAudioSystem::setForceUse(AUDIO_POLICY_FORCE_FOR_RECORD,
                                     AUDIO_POLICY_FORCE_NONE);
      }
    }
  } else if (!strcmp(aTopic, BLUETOOTH_A2DP_STATUS_CHANGED_ID)) {
    if (!isConnected && mA2dpSwitchDone) {
      RefPtr<AudioManager> self = this;
      nsCOMPtr<nsIRunnable> runnable = NS_NewRunnableFunction(
          "AudioManager::HandleBluetoothStatusChanged",
          [self, isConnected, aAddress]() {
            if (self->mA2dpSwitchDone) {
              return;
            }
            self->UpdateDeviceConnectionState(
                isConnected, AUDIO_DEVICE_OUT_BLUETOOTH_A2DP, aAddress);
            self->SetParameters("bluetooth_enabled=false");
            self->SetParameters("A2dpSuspended=true");
            self->mA2dpSwitchDone = true;
          });
      MessageLoop::current()->PostDelayedTask(runnable.forget(), 1000);

      mA2dpSwitchDone = false;
    } else {
      UpdateDeviceConnectionState(isConnected, AUDIO_DEVICE_OUT_BLUETOOTH_A2DP,
                                  aAddress);
      SetParameters("bluetooth_enabled=true");
      SetParameters("A2dpSuspended=false");
      mA2dpSwitchDone = true;
      if (GonkAudioSystem::getForceUse(AUDIO_POLICY_FORCE_FOR_MEDIA) ==
          AUDIO_POLICY_FORCE_NO_BT_A2DP) {
        GonkAudioSystem::setForceUse(AUDIO_POLICY_FORCE_FOR_MEDIA,
                                     AUDIO_POLICY_FORCE_NONE);
      }
    }
    mBluetoothA2dpEnabled = isConnected;
  } else if (!strcmp(aTopic, BLUETOOTH_HFP_STATUS_CHANGED)) {
    UpdateDeviceConnectionState(
        isConnected, AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET, aAddress);
    UpdateDeviceConnectionState(
        isConnected, AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET, aAddress);
  } else if (!strcmp(aTopic, BLUETOOTH_HFP_NREC_STATUS_CHANGED_ID)) {
    // TODO: (Bug 880785) Replace <unknown> with remote Bluetooth device name
    auto* hfp = static_cast<BluetoothHfpManagerBase*>(aSubject);
    const char* state = hfp->IsNrecEnabled() ? "on" : "off";
    SetParameters("bt_headset_name=<unknown>;bt_headset_nrec=%s", state);
  } else if (!strcmp(aTopic, BLUETOOTH_HFP_WBS_STATUS_CHANGED_ID)) {
    auto* hfp = static_cast<BluetoothHfpManagerBase*>(aSubject);
    const char* state = hfp->IsWbsEnabled() ? "on" : "off";
    SetParameters("bt_wbs=%s", state);
  }
#endif
}

nsresult AudioManager::Observe(nsISupports* aSubject, const char* aTopic,
                               const char16_t* aData) {
  if ((strcmp(aTopic, BLUETOOTH_SCO_STATUS_CHANGED) == 0) ||
      (strcmp(aTopic, BLUETOOTH_HFP_STATUS_CHANGED) == 0) ||
      (strcmp(aTopic, BLUETOOTH_HFP_NREC_STATUS_CHANGED_ID) == 0) ||
      (strcmp(aTopic, BLUETOOTH_HFP_WBS_STATUS_CHANGED_ID) == 0) ||
      (strcmp(aTopic, BLUETOOTH_A2DP_STATUS_CHANGED_ID) == 0)) {
    nsCString address = NS_ConvertUTF16toUTF8(nsDependentString(aData));
    if (address.IsEmpty()) {
      NS_WARNING(nsPrintfCString("Invalid address of %s", aTopic).get());
      return NS_ERROR_FAILURE;
    }

    HandleBluetoothStatusChanged(aSubject, aTopic, address);
    return NS_OK;
  }
#ifdef PRODUCT_MANUFACTURER_MTK
  // Notify screen state to audio HAL in order to save power when screen is off.
  else if (!strcmp(aTopic, SCREEN_STATE_CHANGED)) {
    if (u"on"_ns.Equals(aData)) {
      SetParameters("screen_state=on");
    } else {
      SetParameters("screen_state=off");
    }
    return NS_OK;
  }
#endif

  NS_WARNING("Unexpected topic in AudioManager");
  return NS_ERROR_FAILURE;
}

static void NotifyHeadphonesStatus(hal::SwitchState aState) {
  const char16_t* status;
  switch (aState) {
    case hal::SWITCH_STATE_OFF:
      status = HEADPHONES_STATUS_OFF;
      break;
    case hal::SWITCH_STATE_HEADSET:
      status = HEADPHONES_STATUS_HEADSET;
      break;
    case hal::SWITCH_STATE_HEADPHONE:
      status = HEADPHONES_STATUS_HEADPHONE;
      break;
    case hal::SWITCH_STATE_LINEOUT:
      status = HEADPHONES_STATUS_LINEOUT;
      break;
    default:
      status = HEADPHONES_STATUS_UNKNOWN;
      break;
  }

  nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
  if (obs) {
    obs->NotifyObservers(nullptr, HEADPHONES_STATUS_CHANGED, status);
  } else {
    LOGE("Failed to get observer service when notifying headpnone status");
  }
}

class HeadphoneSwitchObserver : public hal::SwitchObserver {
 public:
  void Notify(const hal::SwitchEvent& aEvent) {
    RefPtr<AudioManager> audioManager = AudioManager::GetInstance();
    MOZ_ASSERT(audioManager);
    audioManager->HandleHeadphoneSwitchEvent(aEvent);
  }
};

void AudioManager::HandleHeadphoneSwitchEvent(const hal::SwitchEvent& aEvent) {
  // For more information, see bug 29237.
  // Holds the wakelock for making sure that gecko can do all the needed things
  // before system sleeps.
  //
  // Because of the switch events will be passed from
  // InputReaderThread -> main thread -> IO thread -> main
  // thread(HeadphoneSwitchObserver) I think adding wakelock on every thread is
  // not a good way. So that I decide to hold the wakelock here first. If we
  // still get problems, we may need to add more wakelocks on other threads.
  CreateWakeLock();

  NotifyHeadphonesStatus(aEvent.status());
  // When user pulled out the headset, a delay of routing here can avoid the
  // leakage of audio from speaker.
  if (aEvent.status() == hal::SWITCH_STATE_OFF && mSwitchDone) {
    // When system is in sleep mode and user unplugs the headphone, we need to
    // hold the wakelock here, or the delayed task will not be executed.
    RefPtr<AudioManager> self = this;
    nsCOMPtr<nsIRunnable> runnable = NS_NewRunnableFunction(
        "AudioManager::HandleHeadphoneSwitchEvent", [self]() {
          if (self->mSwitchDone) {
            // The headset was inserted again, so skip this task.
            return;
          }
          self->UpdateHeadsetConnectionState(hal::SWITCH_STATE_OFF);
          self->mSwitchDone = true;
          self->ReleaseWakeLock();
        });
    MessageLoop::current()->PostDelayedTask(runnable.forget(), 1000);
    mSwitchDone = false;
  } else if (aEvent.status() != hal::SWITCH_STATE_OFF) {
    UpdateHeadsetConnectionState(aEvent.status());
    mSwitchDone = true;
  }
  // Handle the coexistence of a2dp / headset device, latest one wins.
  if (aEvent.status() != hal::SWITCH_STATE_OFF && mBluetoothA2dpEnabled) {
    GonkAudioSystem::setForceUse(AUDIO_POLICY_FORCE_FOR_MEDIA,
                                 AUDIO_POLICY_FORCE_NO_BT_A2DP);
  } else if (GonkAudioSystem::getForceUse(AUDIO_POLICY_FORCE_FOR_MEDIA) ==
             AUDIO_POLICY_FORCE_NO_BT_A2DP) {
    GonkAudioSystem::setForceUse(AUDIO_POLICY_FORCE_FOR_MEDIA,
                                 AUDIO_POLICY_FORCE_NONE);
  }
  if (aEvent.status() != hal::SWITCH_STATE_OFF) {
    ReleaseWakeLock();
  }
}

void AudioManager::CreateWakeLock() {
  RefPtr<dom::power::PowerManagerService> pmService =
      dom::power::PowerManagerService::GetInstance();
  NS_ENSURE_TRUE_VOID(pmService);

  if (mWakeLock) {
    // If the user inserts the headset before the SWITCH_STATE_OFF runnable is
    // executed, we may hit his case.
    return;
  }

  ErrorResult rv;
  mWakeLock = pmService->NewWakeLock(u"cpu"_ns, nullptr, rv);
}

void AudioManager::ReleaseWakeLock() {
  if (!mWakeLock) {
    NS_WARNING("ReleaseWakeLock mWakeLock is null");
    return;
  }

  ErrorResult rv;
  mWakeLock->Unlock(rv);
  mWakeLock = nullptr;
}

static StaticRefPtr<AudioManager> sAudioManager;

AudioManager::AudioManager()
    : mObserver(new HeadphoneSwitchObserver()),
      mAudioPortCallbackHolder(new AudioPortCallbackHolder()) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!sAudioManager);

  // Create VolumeStreamStates
  for (int i = AUDIO_STREAM_MIN; i < AUDIO_STREAM_PUBLIC_CNT; i++) {
    auto streamType = static_cast<audio_stream_type_t>(i);
    auto streamState = MakeUnique<VolumeStreamState>(*this, streamType);
    mStreamStates.AppendElement(std::move(streamState));
  }

  Init();
}

void AudioManager::Init() {
  // Register AudioSystem callbacks.
  GonkAudioSystem::setErrorCallback(BinderDeadCallback);
  GonkAudioSystem::addAudioPortCallback(mAudioPortCallbackHolder->Callback());

  // Gecko only control stream volume not master so set to default value
  // directly.
  GonkAudioSystem::setMasterVolume(1.0);

  // Android 10 introduces new rules for sharing audio input. This call can
  // prevent AudioPolicyService from treating us as assistant app and
  // incorrectly muting our audio input because we don't meet some criteria of
  // assistant app.
  GonkAudioSystem::setAssistantUid(AUDIO_UID_INVALID);

  // If this is not set, AUDIO_STREAM_ENFORCED_AUDIBLE will be mapped to
  // AUDIO_STREAM_MUSIC inside AudioPolicyManager.
  GonkAudioSystem::setForceUse(AUDIO_POLICY_FORCE_FOR_SYSTEM,
                               AUDIO_POLICY_FORCE_SYSTEM_ENFORCED);

  // Initialize mStreamStates, which calls AudioSystem::initStreamVolume().
  for (auto& streamState : mStreamStates) {
    streamState->InitStreamVolume();
  }

  // Initialize stream volumes with default values
  for (int i = AUDIO_STREAM_MIN; i < AUDIO_STREAM_PUBLIC_CNT; i++) {
    auto streamType = static_cast<audio_stream_type_t>(i);
    uint32_t volIndex = sDefaultStreamVolumeTbl[streamType];
    SetStreamVolumeForDevice(streamType, volIndex, AUDIO_DEVICE_OUT_DEFAULT);
  }

  RegisterSwitchObserver(hal::SWITCH_HEADPHONES, mObserver.get());
  // Initialize headhone/heaset status
  UpdateHeadsetConnectionState(
      hal::GetCurrentSwitchState(hal::SWITCH_HEADPHONES));
  NotifyHeadphonesStatus(hal::GetCurrentSwitchState(hal::SWITCH_HEADPHONES));

  RegisterSwitchObserver(hal::SWITCH_LINEOUT, mObserver.get());
  // Initialize lineout status
  UpdateHeadsetConnectionState(hal::GetCurrentSwitchState(hal::SWITCH_LINEOUT));
  NotifyHeadphonesStatus(hal::GetCurrentSwitchState(hal::SWITCH_LINEOUT));

  // Get the initial audio settings from DB during boot up.
  ReadAudioSettings();

  // Register to observer service.
  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  if (obs) {
    auto addObserver = [this, &obs](const char* aTopic) {
      if (NS_FAILED(obs->AddObserver(this, aTopic, false))) {
        LOGE("Failed to add %s observer", aTopic);
      }
    };
    addObserver(BLUETOOTH_SCO_STATUS_CHANGED);
    addObserver(BLUETOOTH_A2DP_STATUS_CHANGED_ID);
    addObserver(BLUETOOTH_HFP_STATUS_CHANGED);
    addObserver(BLUETOOTH_HFP_NREC_STATUS_CHANGED_ID);
    addObserver(BLUETOOTH_HFP_WBS_STATUS_CHANGED_ID);
#ifdef PRODUCT_MANUFACTURER_MTK
    addObserver(SCREEN_STATE_CHANGED);
#endif
  } else {
    LOGE("Failed to get observer service when adding observer");
  }

  // Add volume change observer.
  nsCOMPtr<nsISettingsManager> settingsManager =
      do_GetService(SETTINGS_MANAGER);
  if (settingsManager) {
    auto callback = MakeRefPtr<GenericSidlCallback>(__func__);
    mAudioSettingsObserver = MakeRefPtr<AudioSettingsObserver>();
    for (const auto& name : AudioSettingNames(false)) {
      settingsManager->AddObserver(name, mAudioSettingsObserver, callback);
    }
  } else {
    LOGE("Failed to get settings manager when adding observer");
  }

#ifdef MOZ_B2G_RIL
  char value[PROPERTY_VALUE_MAX];
  property_get("ro.moz.mute.call.to_ril", value, "false");
  if (!strcmp(value, "true")) {
    mMuteCallToRIL = true;
  }
#endif
}

AudioManager::~AudioManager() {
  MOZ_ASSERT(!sAudioManager);

  GonkAudioSystem::setErrorCallback(nullptr);
  GonkAudioSystem::removeAudioPortCallback(
      mAudioPortCallbackHolder->Callback());
  hal::UnregisterSwitchObserver(hal::SWITCH_HEADPHONES, mObserver.get());
  hal::UnregisterSwitchObserver(hal::SWITCH_LINEOUT, mObserver.get());

  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  if (obs) {
    auto removeObserver = [this, &obs](const char* aTopic) {
      if (NS_FAILED(obs->RemoveObserver(this, aTopic))) {
        LOGE("Failed to remove %s observer", aTopic);
      }
    };
    removeObserver(BLUETOOTH_SCO_STATUS_CHANGED);
    removeObserver(BLUETOOTH_A2DP_STATUS_CHANGED_ID);
    removeObserver(BLUETOOTH_HFP_STATUS_CHANGED);
    removeObserver(BLUETOOTH_HFP_NREC_STATUS_CHANGED_ID);
    removeObserver(BLUETOOTH_HFP_WBS_STATUS_CHANGED_ID);
#ifdef PRODUCT_MANUFACTURER_MTK
    removeObserver(SCREEN_STATE_CHANGED);
#endif
  } else {
    LOGE("Failed to get observer service when removing observer");
  }

  // Remove volume change observer.
  nsCOMPtr<nsISettingsManager> settingsManager =
      do_GetService(SETTINGS_MANAGER);
  if (settingsManager) {
    auto callback = MakeRefPtr<GenericSidlCallback>(__func__);
    for (const auto& name : AudioSettingNames(false)) {
      settingsManager->RemoveObserver(name, mAudioSettingsObserver, callback);
    }
  } else {
    LOGE("Failed to get settings manager when removing observer");
  }
}

already_AddRefed<AudioManager> AudioManager::GetInstance() {
  // Avoid createing AudioManager from content process.
  if (!XRE_IsParentProcess()) {
    MOZ_CRASH("Non-chrome processes should not get here.");
  }

  MOZ_ASSERT(NS_IsMainThread());

  // Avoid createing multiple AudioManager instance inside main process.
  if (!sAudioManager) {
    sAudioManager = new AudioManager();
    ClearOnShutdown(&sAudioManager);
  }

  RefPtr<AudioManager> audioMgr = sAudioManager.get();
  return audioMgr.forget();
}

NS_IMETHODIMP
AudioManager::GetMicrophoneMuted(bool* aMicrophoneMuted) {
#ifdef MOZ_B2G_RIL
  if (mMuteCallToRIL) {
    // Simply return cached mIsMicMuted if mute call go via RIL.
    *aMicrophoneMuted = mIsMicMuted;
    return NS_OK;
  }
#endif

  status_t err = GonkAudioSystem::isMicrophoneMuted(aMicrophoneMuted);
  return ToNsresult(err);
}

NS_IMETHODIMP
AudioManager::SetMicrophoneMuted(bool aMicrophoneMuted) {
#ifdef MOZ_B2G_RIL
  if (mMuteCallToRIL) {
    // Extra mute request to RIL for specific platform.
    nsCOMPtr<nsIRadioInterfaceLayer> ril = do_GetService("@mozilla.org/ril;1");
    NS_ENSURE_TRUE(ril, NS_ERROR_FAILURE);
    ril->SetMicrophoneMuted(aMicrophoneMuted);
    mIsMicMuted = aMicrophoneMuted;
    return NS_OK;
  }
#endif

  status_t err = GonkAudioSystem::muteMicrophone(aMicrophoneMuted);
  return ToNsresult(err);
}

NS_IMETHODIMP
AudioManager::GetPhoneState(int32_t* aState) {
  *aState = mPhoneState;
  return NS_OK;
}

NS_IMETHODIMP
AudioManager::SetPhoneState(int32_t aState) {
  auto state = static_cast<audio_mode_t>(aState);
  if (mPhoneState == state) {
    return NS_OK;
  }

  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  if (obs) {
    obs->NotifyObservers(nullptr, "phone-state-changed",
                         IntToString<int32_t>(aState).get());
  } else {
    LOGE("Failed to get observer service when notifying phone state");
  }

  status_t err = GonkAudioSystem::setPhoneState(state);
  if (err != android::OK) {
    return ToNsresult(err);
  }

  MaybeWriteVolumeSettings();
  mPhoneState = state;
  return NS_OK;
}

NS_IMETHODIMP
AudioManager::GetHeadsetState(int32_t* aHeadsetState) {
  MOZ_ASSERT(aHeadsetState);

  hal::SwitchState state = GetCurrentSwitchState(hal::SWITCH_HEADPHONES);
  if (state == hal::SWITCH_STATE_HEADSET) {
    *aHeadsetState = nsIAudioManager::HEADSET_STATE_HEADSET;
  } else if (state == hal::SWITCH_STATE_HEADPHONE) {
    *aHeadsetState = nsIAudioManager::HEADSET_STATE_HEADPHONE;
  } else if (state == hal::SWITCH_STATE_OFF) {
    *aHeadsetState = nsIAudioManager::HEADSET_STATE_OFF;
  } else {
    *aHeadsetState = nsIAudioManager::HEADSET_STATE_UNKNOWN;
  }

  return NS_OK;
}

NS_IMETHODIMP
AudioManager::SetTtyMode(uint16_t aTtyMode) {
  if (aTtyMode == nsIAudioManager::TTY_MODE_FULL) {
    SetParameters("tty_mode=tty_full");
  } else if (aTtyMode == nsIAudioManager::TTY_MODE_HCO) {
    SetParameters("tty_mode=tty_hco");
  } else if (aTtyMode == nsIAudioManager::TTY_MODE_VCO) {
    SetParameters("tty_mode=tty_vco");
  } else {
    SetParameters("tty_mode=tty_off");
  }
  return NS_OK;
}

NS_IMETHODIMP
AudioManager::SetForceForUse(int32_t aUsage, int32_t aForce) {
  auto usage = static_cast<audio_policy_force_use_t>(aUsage);
  auto config = static_cast<audio_policy_forced_cfg_t>(aForce);
  status_t err = GonkAudioSystem::setForceUse(usage, config);

  // AudioPortListUpdate may not be triggered after setting force use, so
  // manually update volume settings here.
  MaybeWriteVolumeSettings();

  if (usage == AUDIO_POLICY_FORCE_FOR_MEDIA) {
    SetFmRouting();
  }
  return ToNsresult(err);
}

NS_IMETHODIMP
AudioManager::GetForceForUse(int32_t aUsage, int32_t* aForce) {
  auto usage = static_cast<audio_policy_force_use_t>(aUsage);
  *aForce = GonkAudioSystem::getForceUse(usage);
  return NS_OK;
}

NS_IMETHODIMP
AudioManager::GetFmRadioAudioEnabled(bool* aEnabled) {
  *aEnabled = IsFmOutConnected();
  return NS_OK;
}

void AudioManager::SetFmRouting() {
  // Mute FM when switching output paths. This can avoid pop noise caused by the
  // time gap between setting new path and setting the volume of the new path.
  if (IsFmOutConnected()) {
    SetFmMuted(true);
  }
#if defined(PRODUCT_MANUFACTURER_QUALCOMM)
  // Setting "fm_routing" restarts FM audio path, so only do it when FM audio is
  // already enabled.
  if (IsFmOutConnected()) {
    SetParameters("fm_routing=%d", GetDeviceForFm() | AUDIO_DEVICE_OUT_FM);
  }
#elif defined(PRODUCT_MANUFACTURER_SPRD)
  // Sync force use between MEDIA and FM
  auto force = GonkAudioSystem::getForceUse(AUDIO_POLICY_FORCE_FOR_MEDIA);
  GonkAudioSystem::setForceUse(AUDIO_POLICY_FORCE_FOR_FM, force);
#elif defined(PRODUCT_MANUFACTURER_MTK)
  /* FIXME
  // Sync force use between MEDIA and FM
  auto force = GonkAudioSystem::getForceUse(AUDIO_POLICY_FORCE_FOR_MEDIA);
  GonkAudioSystem::setForceUse(AUDIO_POLICY_FORCE_FOR_PROPRIETARY, force);
  */
#else
  MOZ_CRASH("FM radio not supported");
#endif
  if (IsFmOutConnected()) {
    SetFmMuted(false);
  }
}

void AudioManager::UpdateFmVolume() {
#if defined(PRODUCT_MANUFACTURER_QUALCOMM)
  audio_devices_t device = GetDeviceForFm();
  uint32_t volIndex = mStreamStates[AUDIO_STREAM_MUSIC]->GetVolumeIndex(device);
  float decibel =
      GonkAudioSystem::getStreamVolumeDB(AUDIO_STREAM_MUSIC, volIndex, device);
  float volume = exp(decibel * 0.115129f);
  SetParameters("fm_volume=%f", volume);
#elif defined(PRODUCT_MANUFACTURER_SPRD)
  audio_devices_t device = GetDeviceForFm();
  uint32_t volIndex = mStreamStates[AUDIO_STREAM_MUSIC]->GetVolumeIndex(device);
  SetParameters("FM_Volume=%d", volIndex);
#endif
}

void AudioManager::SetFmMuted(bool aMuted) {
#if defined(PRODUCT_MANUFACTURER_QUALCOMM)
  // Update FM volume before unmuted.
  if (!aMuted) {
    UpdateFmVolume();
  }
  SetParameters("fm_mute=%d", aMuted);
#elif defined(PRODUCT_MANUFACTURER_SPRD)
  if (aMuted) {
    // Is there a mute command?
    SetParameters("FM_Volume=0");
  } else {
    UpdateFmVolume();
  }
#endif
  // MTK platform handles FM volume internally, so no need to set muted here.
}

NS_IMETHODIMP
AudioManager::SetFmRadioAudioEnabled(bool aEnabled) {
  // Mute FM audio first, and unmute it after the correct routing and volume are
  // set.
  if (aEnabled) {
    SetFmMuted(true);
  }
#if defined(PRODUCT_MANUFACTURER_QUALCOMM)
  if (aEnabled) {
    if (IsFmOutConnected()) {
      // Stop FM audio and start it again with correct routing.
      SetFmRouting();
    } else {
      // Just start FM audio with correct routing.
      SetParameters("handle_fm=%d", GetDeviceForFm() | AUDIO_DEVICE_OUT_FM);
    }
  } else {
    // Remove AUDIO_DEVICE_OUT_FM to stop FM audio. The device cannot be 0.
    SetParameters("handle_fm=%d", GetDeviceForFm());
  }
#elif defined(PRODUCT_MANUFACTURER_SPRD)
  UpdateDeviceConnectionState(aEnabled, AUDIO_DEVICE_OUT_FM_HEADSET);
  UpdateDeviceConnectionState(aEnabled, AUDIO_DEVICE_OUT_FM_SPEAKER);
#elif defined(PRODUCT_MANUFACTURER_MTK)
  UpdateDeviceConnectionState(aEnabled, AUDIO_DEVICE_IN_FM_TUNER);
#else
  MOZ_CRASH("FM radio not supported");
#endif
  if (aEnabled) {
    SetFmMuted(false);
  }
  return NS_OK;
}

NS_IMETHODIMP
AudioManager::SetFmRadioContentVolume(float aVolume) {
  mFmContentVolume = std::max(aVolume, 0.0f);
  UpdateFmVolume();
  return NS_OK;
}

NS_IMETHODIMP
AudioManager::SetAudioChannelVolume(uint32_t aChannel, uint32_t aIndex) {
  if (aChannel >= NUMBER_OF_AUDIO_CHANNELS) {
    return NS_ERROR_INVALID_ARG;
  }

  return SetStreamVolumeIndex(sChannelStreamTbl[aChannel], aIndex);
}

NS_IMETHODIMP
AudioManager::GetAudioChannelVolume(uint32_t aChannel, uint32_t* aIndex) {
  if (aChannel >= NUMBER_OF_AUDIO_CHANNELS) {
    return NS_ERROR_INVALID_ARG;
  }

  if (!aIndex) {
    return NS_ERROR_NULL_POINTER;
  }

  return GetStreamVolumeIndex(sChannelStreamTbl[aChannel], aIndex);
}

NS_IMETHODIMP
AudioManager::GetMaxAudioChannelVolume(uint32_t aChannel, uint32_t* aMaxIndex) {
  if (aChannel >= NUMBER_OF_AUDIO_CHANNELS) {
    return NS_ERROR_INVALID_ARG;
  }

  if (!aMaxIndex) {
    return NS_ERROR_NULL_POINTER;
  }

  *aMaxIndex = mStreamStates[sChannelStreamTbl[aChannel]]->GetMaxIndex();
  return NS_OK;
}

nsresult AudioManager::ValidateVolumeIndex(audio_stream_type_t aStream,
                                           uint32_t aIndex) const {
  if (aStream <= AUDIO_STREAM_DEFAULT || aStream >= AUDIO_STREAM_PUBLIC_CNT) {
    return NS_ERROR_INVALID_ARG;
  }

  auto& streamState = mStreamStates[aStream];
  if (aIndex < streamState->GetMinIndex() ||
      aIndex > streamState->GetMaxIndex()) {
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

nsresult AudioManager::SetStreamVolumeForDevice(audio_stream_type_t aStream,
                                                uint32_t aIndex,
                                                audio_devices_t aDevice) {
  if (ValidateVolumeIndex(aStream, aIndex) != NS_OK) {
    return NS_ERROR_INVALID_ARG;
  }

  audio_stream_type_t streamAlias = sStreamVolumeAliasTbl[aStream];
  auto& streamState = mStreamStates[streamAlias];
  return streamState->SetVolumeIndexToAliasStreams(aIndex, aDevice);
}

nsresult AudioManager::SetStreamVolumeIndex(audio_stream_type_t aStream,
                                            uint32_t aIndex) {
  if (ValidateVolumeIndex(aStream, aIndex) != NS_OK) {
    return NS_ERROR_INVALID_ARG;
  }

  audio_stream_type_t streamAlias = sStreamVolumeAliasTbl[aStream];
  for (int i = AUDIO_STREAM_MIN; i < AUDIO_STREAM_PUBLIC_CNT; i++) {
    if (streamAlias == sStreamVolumeAliasTbl[i]) {
      nsresult rv = mStreamStates[i]->SetVolumeIndexToActiveDevices(aIndex);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
    }
  }
  MaybeWriteVolumeSettings();
  return NS_OK;
}

nsresult AudioManager::GetStreamVolumeIndex(audio_stream_type_t aStream,
                                            uint32_t* aIndex) {
  if (!aIndex) {
    return NS_ERROR_INVALID_ARG;
  }

  if (aStream <= AUDIO_STREAM_DEFAULT || aStream >= AUDIO_STREAM_PUBLIC_CNT) {
    return NS_ERROR_INVALID_ARG;
  }

  *aIndex = mStreamStates[aStream]->GetVolumeIndex();
  return NS_OK;
}

void AudioManager::ReadAudioSettings() {
  nsCOMPtr<nsISettingsManager> settingsManager =
      do_GetService(SETTINGS_MANAGER);
  if (!settingsManager) {
    LOGE("%s, failed to get settings manager", __func__);
    return;
  }

  auto callback = MakeRefPtr<AudioSettingsGetCallback>();
  settingsManager->GetBatch(AudioSettingNames(true), callback);
}

void AudioManager::ReadAudioSettingsFinished() {
  mIsVolumeInited = true;
  MaybeWriteVolumeSettings(true);
}

void AudioManager::MaybeWriteVolumeSettings(bool aForce) {
  if (!mIsVolumeInited) {
    return;
  }

  // Send events to update the Gaia volumes
  nsTArray<RefPtr<nsISettingInfo>> settings;

  for (const auto& data : gVolumeData) {
    auto& streamState = mStreamStates[data.mStreamType];
    bool isVolumeUpdated = streamState->IsDevicesChanged() &&
                           streamState->IsDeviceSpecificVolume();

    if (aForce || isVolumeUpdated) {
      auto& name = data.mChannelName;
      auto volIndex = streamState->GetVolumeIndex();
      auto value = IntToString<uint32_t>(volIndex);
      RefPtr<nsISettingInfo> settingInfo = new SettingInfo(name, value);
      settings.AppendElement(settingInfo);
    }
  }

  // For reducing the code dependency, Gaia doesn't need to know the device
  // volume, it only need to care about different volume categories. However, we
  // need to send the setting volume to the permanent database, so that we can
  // store the volume setting even if the phone reboots.
  for (const auto& data : gVolumeData) {
    auto& streamState = mStreamStates[data.mStreamType];
    auto devices = streamState->GetDevicesWithVolumeChange();

    for (const auto& deviceInfo : kAudioDeviceInfos) {
      if (devices.count(deviceInfo.value)) {
        // append device suffix to the channel name
        nsAutoString name = data.mChannelName + u"."_ns + deviceInfo.tag;
        auto volIndex = streamState->GetVolumeIndex(deviceInfo.value);
        auto value = IntToString<uint32_t>(volIndex);
        RefPtr<nsISettingInfo> settingInfo = new SettingInfo(name, value);
        settings.AppendElement(settingInfo);
      }
    }
  }

  if (settings.Length() > 0) {
    nsCOMPtr<nsISettingsManager> settingsManager =
        do_GetService(SETTINGS_MANAGER);
    if (settingsManager) {
      auto callback = MakeRefPtr<GenericSidlCallback>(__func__);
      settingsManager->Set(settings, callback);
    } else {
      LOGE("Fail to get SETTINGS_MANAGER to set the volume.");
    }
  }

  // Clear changed flags
  for (const auto& data : gVolumeData) {
    audio_stream_type_t streamType = data.mStreamType;
    mStreamStates[streamType]->ClearDevicesChanged();
    mStreamStates[streamType]->ClearDevicesWithVolumeChange();
  }
}

void AudioManager::OnAudioSettingChanged(const nsAString& aName,
                                         const nsAString& aValue) {
  LOG("%s, {%s: %s}", __func__, NS_ConvertUTF16toUTF8(aName).get(),
      NS_ConvertUTF16toUTF8(aValue).get());

  if (StringBeginsWith(aName, u"audio.volume."_ns)) {
    // The key from FE in the first booting would be like
    // "audio.volumes.content" (without device suffix). For such cases, we
    // have to set the volumes of all devices with this value. If not, the
    // following stages will set the volumes by Gecko's defaults that could
    // conflict with the UX specifications.
    audio_stream_type_t stream;
    audio_devices_t device;
    uint32_t volIndex;
    nsresult rv =
        ParseVolumeSetting(aName, aValue, &stream, &device, &volIndex);
    if (NS_FAILED(rv)) {
      LOGE("%s, failed to parse volume setting, {%s: %s}", __func__,
           NS_ConvertUTF16toUTF8(aName).get(),
           NS_ConvertUTF16toUTF8(aValue).get());
      return;
    }

    rv = device == AUDIO_DEVICE_NONE
             ? SetStreamVolumeIndex(stream, volIndex)
             : SetStreamVolumeForDevice(stream, volIndex, device);
    if (NS_FAILED(rv)) {
      LOGE("%s, failed to set volume, {%s: %s}", __func__,
           NS_ConvertUTF16toUTF8(aName).get(),
           NS_ConvertUTF16toUTF8(aValue).get());
      return;
    }
  } else if (aName.Equals(u"accessibility.force_mono_audio"_ns)) {
    mMasterMono = aValue.Equals(u"true"_ns);
    GonkAudioSystem::setMasterMono(mMasterMono);
  } else if (aName.Equals(u"accessibility.volume_balance"_ns)) {
    nsresult rv;
    int32_t balance = aValue.ToInteger(&rv);
    if (NS_FAILED(rv)) {
      LOGE("%s, incorrect balance value, {%s: %s}", __func__,
           NS_ConvertUTF16toUTF8(aName).get(),
           NS_ConvertUTF16toUTF8(aValue).get());
      return;
    }
    mMasterBalance = std::clamp(balance, 0, 100) / 100.0f;
    GonkAudioSystem::setMasterBalance(mMasterBalance);
  }
}

nsresult AudioManager::ParseVolumeSetting(const nsAString& aName,
                                          const nsAString& aValue,
                                          audio_stream_type_t* aStream,
                                          audio_devices_t* aDevice,
                                          uint32_t* aVolIndex) {
  nsresult rv;
  uint32_t volIndex = aValue.ToInteger(&rv);
  if (NS_FAILED(rv)) {
    return rv;
  }

  for (const auto& [channelName, streamType] : gVolumeData) {
    if (StringBeginsWith(aName, channelName)) {
      // Found a matched channe name. Check if any device suffix presents.
      audio_devices_t device = AUDIO_DEVICE_NONE;
      for (const auto& [deviceTag, deviceValue] : kAudioDeviceInfos) {
        if (StringEndsWith(aName, deviceTag)) {
          device = deviceValue;
          break;
        }
      }
      *aStream = streamType;
      *aDevice = device;
      *aVolIndex = volIndex;
      return NS_OK;
    }
  }
  return NS_ERROR_FAILURE;
}

nsTArray<nsString> AudioManager::AudioSettingNames(bool aInitializing) {
  nsTArray<nsString> names;
  for (const auto& [channelName, streamType] : gVolumeData) {
    // Get the setting of each channel name. FE uses only the channel name
    // (without device suffix) as the key.
    names.AppendElement(channelName);

    // At initializing stage, if this stream type uses device-specific volume
    // setting, also get the settings with device suffix. These settings are
    // only used by Gecko internally, and only need to be read once after each
    // boot.
    if (aInitializing && mStreamStates[streamType]->IsDeviceSpecificVolume()) {
      for (const auto& [deviceTag, deviceValue] : kAudioDeviceInfos) {
        names.AppendElement(channelName + u"."_ns + deviceTag);
      }
    }
  }

  names.AppendElement(u"accessibility.force_mono_audio"_ns);
  names.AppendElement(u"accessibility.volume_balance"_ns);
  return names;
}

audio_devices_t AudioManager::GetDeviceForStream(audio_stream_type_t aStream) {
  auto devices = GonkAudioSystem::getDeviceTypesForStream(aStream);
  auto device = devices.size() ? *devices.begin() : AUDIO_DEVICE_NONE;

  // Consider force use speaker case.
  if (GonkAudioSystem::getForceUse(AUDIO_POLICY_FORCE_FOR_MEDIA) ==
      AUDIO_POLICY_FORCE_SPEAKER) {
    device = AUDIO_DEVICE_OUT_SPEAKER;
  }

  // See android AudioService.getDeviceForStream().
  // AudioPolicyManager expects it.
  // See also android AudioPolicy Volume::getDeviceForVolume().
  if (devices.size() > 1) {
    // Multiple device selection.
    if (devices.count(AUDIO_DEVICE_OUT_SPEAKER)) {
      device = AUDIO_DEVICE_OUT_SPEAKER;
    } else if (devices.count(AUDIO_DEVICE_OUT_HDMI_ARC)) {
      device = AUDIO_DEVICE_OUT_HDMI_ARC;
    } else if (devices.count(AUDIO_DEVICE_OUT_SPDIF)) {
      device = AUDIO_DEVICE_OUT_SPDIF;
    } else if (devices.count(AUDIO_DEVICE_OUT_AUX_LINE)) {
      device = AUDIO_DEVICE_OUT_AUX_LINE;
    } else {
      for (auto a2dp : AUDIO_DEVICE_OUT_ALL_A2DP_ARRAY) {
        if (devices.count(a2dp)) {
          device = a2dp;
          break;
        }
      }
    }
  }

  // From android AudioPolicy Volume::getDeviceForVolume().
  if (device == AUDIO_DEVICE_NONE) {
    // this happens when forcing a route update and no track is active on an
    // output. In this case the returned category is not important.
    device = AUDIO_DEVICE_OUT_SPEAKER;
  }

  MOZ_ASSERT(audio_is_output_device(static_cast<audio_devices_t>(device)));
  return device;
}

audio_devices_t AudioManager::GetDeviceForFm() {
  // Assume that FM radio supports the same routing of music stream.
  return GetDeviceForStream(AUDIO_STREAM_MUSIC);
}

AudioManager::VolumeStreamState::VolumeStreamState(
    AudioManager& aManager, audio_stream_type_t aStreamType)
    : mManager(aManager), mStreamType(aStreamType) {
  switch (mStreamType) {
    case AUDIO_STREAM_SYSTEM:
    case AUDIO_STREAM_RING:
    case AUDIO_STREAM_NOTIFICATION:
    case AUDIO_STREAM_ALARM:
    case AUDIO_STREAM_ENFORCED_AUDIBLE:
      mIsDeviceSpecificVolume = false;
      break;
    default:
      break;
  }
}

bool AudioManager::VolumeStreamState::IsDevicesChanged() {
  auto devices = GonkAudioSystem::getDeviceTypesForStream(mStreamType);
  if (devices != mLastDevices) {
    mLastDevices = std::move(devices);
    mIsDevicesChanged = true;
  }
  return mIsDevicesChanged;
}

void AudioManager::VolumeStreamState::ClearDevicesChanged() {
  mIsDevicesChanged = false;
}

void AudioManager::VolumeStreamState::ClearDevicesWithVolumeChange() {
  mDevicesWithVolumeChange.clear();
}

DeviceTypeSet AudioManager::VolumeStreamState::GetDevicesWithVolumeChange() {
  return mDevicesWithVolumeChange;
}

void AudioManager::VolumeStreamState::InitStreamVolume() {
  GonkAudioSystem::initStreamVolume(mStreamType, GetMinIndex(), GetMaxIndex());
}

uint32_t AudioManager::VolumeStreamState::GetMaxIndex() {
  return sMaxStreamVolumeTbl[mStreamType];
}

uint32_t AudioManager::VolumeStreamState::GetMinIndex() {
  return sMinStreamVolumeTbl[mStreamType];
}

uint32_t AudioManager::VolumeStreamState::GetVolumeIndex() {
  audio_devices_t device = mManager.GetDeviceForStream(mStreamType);
  return GetVolumeIndex(device);
}

uint32_t AudioManager::VolumeStreamState::GetVolumeIndex(
    audio_devices_t aDevice) {
  if (mVolumeIndexes.count(aDevice)) {
    return mVolumeIndexes[aDevice];
  }
  if (mVolumeIndexes.count(AUDIO_DEVICE_OUT_DEFAULT)) {
    return mVolumeIndexes[AUDIO_DEVICE_OUT_DEFAULT];
  }
  return 0;
}

nsresult AudioManager::VolumeStreamState::SetVolumeIndexToActiveDevices(
    uint32_t aIndex) {
  audio_devices_t device = mManager.GetDeviceForStream(mStreamType);
  if (mVolumeIndexes.count(device) && mVolumeIndexes[device] == aIndex) {
    // No update.
    return NS_OK;
  }

  // AudioPolicyManager::setStreamVolumeIndex() set volumes of all active
  // devices for stream.
  nsresult rv;
  rv = SetVolumeIndexToConsistentDeviceIfNeeded(aIndex, device);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }
  return NS_OK;
}

nsresult AudioManager::VolumeStreamState::SetVolumeIndexToAliasStreams(
    uint32_t aIndex, audio_devices_t aDevice) {
  if (mVolumeIndexes.count(aDevice) && mVolumeIndexes[aDevice] == aIndex) {
    // No update.
    return NS_OK;
  }

  nsresult rv = SetVolumeIndexToConsistentDeviceIfNeeded(aIndex, aDevice);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  for (int i = AUDIO_STREAM_MIN; i < AUDIO_STREAM_PUBLIC_CNT; i++) {
    if (i != mStreamType && sStreamVolumeAliasTbl[i] == mStreamType) {
      // Rescaling of index is not necessary.
      rv = mManager.mStreamStates[i]->SetVolumeIndexToAliasStreams(aIndex,
                                                                   aDevice);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
    }
  }

  return NS_OK;
}

nsresult
AudioManager::VolumeStreamState::SetVolumeIndexToConsistentDeviceIfNeeded(
    uint32_t aIndex, audio_devices_t aDevice) {
  nsresult rv;

  if (!IsDeviceSpecificVolume()) {
    rv = SetVolumeIndex(aIndex, AUDIO_DEVICE_OUT_DEFAULT);
  } else if (aDevice == AUDIO_DEVICE_OUT_SPEAKER ||
             aDevice == AUDIO_DEVICE_OUT_EARPIECE) {
    // Set AUDIO_DEVICE_OUT_SPEAKER and AUDIO_DEVICE_OUT_EARPIECE to same
    // volume.
    rv = SetVolumeIndex(aIndex, AUDIO_DEVICE_OUT_SPEAKER);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
    rv = SetVolumeIndex(aIndex, AUDIO_DEVICE_OUT_EARPIECE);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
  } else {
    // No alias device
    rv = SetVolumeIndex(aIndex, aDevice);
  }
  return rv;
}

nsresult AudioManager::VolumeStreamState::SetVolumeIndex(
    uint32_t aIndex, audio_devices_t aDevice, bool aUpdateCache) {
  if (aUpdateCache) {
    mVolumeIndexes[aDevice] = aIndex;
    mDevicesWithVolumeChange.insert(aDevice);
  }

  status_t err =
      GonkAudioSystem::setStreamVolumeIndex(mStreamType, aIndex, aDevice);

  // when changing music volume,  also set FMradio volume.Just for SPRD FMradio.
  if ((AUDIO_STREAM_MUSIC == mStreamType) && mManager.IsFmOutConnected()) {
    mManager.UpdateFmVolume();
  }

  return ToNsresult(err);
}

void AudioManager::VolumeStreamState::RestoreVolumeIndexToAllDevices() {
  for (auto [device, index] : mVolumeIndexes) {
    SetVolumeIndex(index, device, /* aUpdateCache */ false);
  }
}

NS_IMETHODIMP
AudioManager::SetHacMode(bool aHacMode) {
  if (aHacMode) {
    SetParameters("HACSetting=ON");
  } else {
    SetParameters("HACSetting=OFF");
  }
  return NS_OK;
}

NS_IMETHODIMP
AudioManager::SetParameters(const nsACString& aKeyValuePairs) {
  status_t err = GonkAudioSystem::setParameters(ToString8(aKeyValuePairs));
  return ToNsresult(err);
}

nsresult AudioManager::SetParameters(const char* aFormat, ...) {
  va_list args;
  va_start(args, aFormat);
  String8 cmd;
  status_t err = cmd.appendFormatV(aFormat, args);
  va_end(args);
  if (err != android::OK) {
    LOGE("Invalid parameter, error status: %d", err);
    return ToNsresult(err);
  }

  err = GonkAudioSystem::setParameters(cmd);
  return ToNsresult(err);
}

nsAutoCString AudioManager::GetParameters(const char* aKeys) {
  auto keyValuePairs = GonkAudioSystem::getParameters(String8(aKeys));
  return nsAutoCString(keyValuePairs.string());
}

} /* namespace gonk */
} /* namespace dom */
} /* namespace mozilla */
