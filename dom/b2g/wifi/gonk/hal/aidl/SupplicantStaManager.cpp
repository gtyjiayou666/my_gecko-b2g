/* Copyright (C) 2020 KAI OS TECHNOLOGIES (HONG KONG) LIMITED. All rights
 * reserved. Copyright 2013 Mozilla Foundation and Mozilla contributors
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


#include <cstddef>
#define LOG_TAG "SupplicantStaManager"

#include "SupplicantStaManager.h"
#include <cutils/properties.h>
#include <utils/Log.h>
#include <string.h>
#include <utils/String8.h>
#include <mozilla/ClearOnShutdown.h>

#define ANY_MAC_STR "any"
#define WPS_DEV_TYPE_LEN 8
#define WPS_DEV_TYPE_DUAL_SMARTPHONE "10-0050F204-5"

#define EVENT_SUPPLICANT_TERMINATING u"SUPPLICANT_TERMINATING"_ns

using ::android::binder::Status;
using ::android::hardware::wifi::supplicant::AnqpInfoId;
using ::android::hardware::wifi::supplicant::BtCoexistenceMode;
using ::android::hardware::wifi::supplicant::DebugLevel;
using ::android::hardware::wifi::supplicant::Hs20AnqpSubtypes;
using ::android::hardware::wifi::supplicant::IfaceInfo;
using ::android::hardware::wifi::supplicant::IfaceType;
using ::android::hardware::wifi::supplicant::ISupplicantStaNetwork;
using ::android::hardware::wifi::supplicant::KeyMgmtMask;
using ::android::hardware::wifi::supplicant::WpsConfigMethods;

static const char HAL_INSTANCE_NAME[] = "default";

mozilla::Mutex SupplicantStaManager::sLock("supplicant-sta");
mozilla::Mutex SupplicantStaManager::sHashLock("supplicant-hash");

static StaticAutoPtr<SupplicantStaManager> sSupplicantManager;

android::String16 getStaIfaceName() {
  std::array<char, PROPERTY_VALUE_MAX> buffer;
  property_get("wifi.interface", buffer.data(), "wlan0");
  return android::String16(buffer.data());
}

SupplicantStaManager::SupplicantStaManager()
    : mServiceManager(nullptr),
      mSupplicant(nullptr),
      mSupplicantStaIface(nullptr),
      mSupplicantStaIfaceCallback(nullptr),
      mServiceManagerDeathRecipient(nullptr),
      mSupplicantDeathRecipient(nullptr),
      mDeathEventHandler(nullptr),
      mDeathRecipientCookie(0) {}

SupplicantStaManager* SupplicantStaManager::Get() {
  MOZ_ASSERT(NS_IsMainThread());

  if (!sSupplicantManager) {
    sSupplicantManager = new SupplicantStaManager();
    ClearOnShutdown(&sSupplicantManager);
  }
  return sSupplicantManager;
}

void SupplicantStaManager::CleanUp() {
  if (sSupplicantManager != nullptr) {
    sSupplicantManager->DeinitInterface();
  }
}

void SupplicantStaManager::ServiceManagerDeathRecipient::serviceDied(
    uint64_t, const android::wp<IBase>&) {
  WIFI_LOGE(LOG_TAG, "IServiceManager HAL died, cleanup instance.");
  MutexAutoLock lock(sLock);

  if (mOuter != nullptr) {
    mOuter->SupplicantServiceDiedHandler(mOuter->mDeathRecipientCookie);
    mOuter->mServiceManager = nullptr;
  }
}

void SupplicantStaManager::SupplicantDeathRecipient::serviceDied(
    uint64_t, const android::wp<IBase>&) {
  WIFI_LOGE(LOG_TAG, "ISupplicant HAL died, cleanup instance.");
  MutexAutoLock lock(sLock);

  if (mOuter != nullptr) {
    mOuter->SupplicantServiceDiedHandler(mOuter->mDeathRecipientCookie);
    mOuter->mSupplicant = nullptr;
  }
}

void SupplicantStaManager::RegisterEventCallback(
    const android::sp<WifiEventCallback>& aCallback) {
  mCallback = aCallback;
}

void SupplicantStaManager::UnregisterEventCallback() { mCallback = nullptr; }

void SupplicantStaManager::RegisterPasspointCallback(
    PasspointEventCallback* aCallback) {
  mPasspointCallback = aCallback;
}

void SupplicantStaManager::UnregisterPasspointCallback() {
  mPasspointCallback = nullptr;
}

Result_t SupplicantStaManager::InitInterface() {
  if (mSupplicant != nullptr) {
    return nsIWifiResult::SUCCESS;
  }
  return InitServiceManager();
}

Result_t SupplicantStaManager::DeinitInterface() { return TearDownInterface(); }

Result_t SupplicantStaManager::InitServiceManager() {
  MutexAutoLock lock(sLock);
  if (mServiceManager != nullptr) {
    // service already existed.
    return nsIWifiResult::SUCCESS;
  }

  mServiceManager =
      ::android::hidl::manager::V1_0::IServiceManager::getService();
  if (mServiceManager == nullptr) {
    WIFI_LOGE(LOG_TAG, "Failed to get HIDL service manager");
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }

  if (mServiceManagerDeathRecipient == nullptr) {
    mServiceManagerDeathRecipient =
        new ServiceManagerDeathRecipient(sSupplicantManager);
  }

  Return<bool> linked =
      mServiceManager->linkToDeath(mServiceManagerDeathRecipient, 0);
  if (!linked || !linked.isOk()) {
    WIFI_LOGE(LOG_TAG, "Error on linkToDeath to IServiceManager");
    SupplicantServiceDiedHandler(mDeathRecipientCookie);
    mServiceManager = nullptr;
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }

  return nsIWifiResult::SUCCESS;
}

Result_t SupplicantStaManager::InitSupplicantInterface() {
  MutexAutoLock lock(sLock);
  mSupplicant = android::waitForVintfService<ISupplicant>();

  if (mSupplicant != nullptr) {
    ALOGE("Found ISupplicant service...");
    Status status;
    status = mSupplicant->registerCallback(this);
    if (!status.isOk()) {
      ALOGE("Failed to register supplicant callback");
      return nsIWifiResult::ERROR_COMMAND_FAILED;
      mSupplicant = nullptr;
    } else {
      ALOGE("Register supplicant callback success");
    }
  } else {
    ALOGE("Can't find ISupplicant service...");
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }
  return nsIWifiResult::SUCCESS;
}

bool SupplicantStaManager::IsInterfaceInitializing() {
  MutexAutoLock lock(sLock);
  return mServiceManager != nullptr;
}

bool SupplicantStaManager::IsInterfaceReady() {
  MutexAutoLock lock(sLock);
  return mSupplicant != nullptr;
}

Result_t SupplicantStaManager::TearDownInterface() {
  MutexAutoLock lock(sLock);

  ModifyConfigurationHash(CLEAN_ALL, mDummyNetworkConfiguration);
  mCurrentNetwork.clear();
  mSupplicant = nullptr;
  mSupplicantStaIface = nullptr;
  mSupplicantStaIfaceCallback = nullptr;

  return nsIWifiResult::SUCCESS;
}

Result_t SupplicantStaManager::GetMacAddress(nsAString& aMacAddress) {
  MutexAutoLock lock(sLock);
  if (!mSupplicantStaIface) {
    return nsIWifiResult::ERROR_INVALID_INTERFACE;
  }

  Status status;
  std::vector<uint8_t> macAddr;
  status = mSupplicantStaIface->getMacAddress(&macAddr);
  if (status.isOk()) {
    std::string addressStr = ConvertMacToString(macAddr);
    nsString address(NS_ConvertUTF8toUTF16(addressStr.c_str()));
    aMacAddress.Assign(address);
  }

  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::GetSupportedFeatures(
    uint32_t& aSupportedFeatures) {
  MutexAutoLock lock(sLock);
  if (!mSupplicantStaIface) {
    return nsIWifiResult::ERROR_INVALID_INTERFACE;
  }

  KeyMgmtMask capabilities;
  Status status;
  status = mSupplicantStaIface->getKeyMgmtCapabilities(&capabilities);

  if (!status.isOk()) {
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }

  if (!!(static_cast<uint32_t>(capabilities) &
         static_cast<uint32_t>(KeyMgmtMask::SAE))) {
    // SAE supported
    aSupportedFeatures |= nsIWifiResult::FEATURE_WPA3_SAE;
  }
  if (!!(static_cast<uint32_t>(capabilities) &
         static_cast<uint32_t>(KeyMgmtMask::SUITE_B_192))) {
    // SUITE_B supported
    aSupportedFeatures |= nsIWifiResult::FEATURE_WPA3_SUITE_B;
  }
  if (!!(static_cast<uint32_t>(capabilities) &
         static_cast<uint32_t>(KeyMgmtMask::OWE))) {
    // OWE supported
    aSupportedFeatures |= nsIWifiResult::FEATURE_OWE;
  }
  if (!!(static_cast<uint32_t>(capabilities) &
         static_cast<uint32_t>(KeyMgmtMask::DPP))) {
    // DPP supported
    aSupportedFeatures |= nsIWifiResult::FEATURE_DPP;
  }
  return nsIWifiResult::SUCCESS;
}

Result_t SupplicantStaManager::GetSupplicantDebugLevel(uint32_t& aLevel) {
  MutexAutoLock lock(sLock);
  if (!mSupplicant) {
    return nsIWifiResult::ERROR_INVALID_INTERFACE;
  }
  DebugLevel debugLevel;
  mSupplicant->getDebugLevel(&debugLevel);
  aLevel = (uint32_t)debugLevel;
  return nsIWifiResult::SUCCESS;
}

Result_t SupplicantStaManager::SetSupplicantDebugLevel(
    SupplicantDebugLevelOptions* aLevel) {
  MutexAutoLock lock(sLock);
  if (!mSupplicant) {
    return nsIWifiResult::ERROR_INVALID_INTERFACE;
  }
  Status status;
  status =
      mSupplicant->setDebugParams(static_cast<DebugLevel>(aLevel->mLogLevel),
                                  aLevel->mShowTimeStamp, aLevel->mShowKeys);
  if (!status.isOk()) {
    WIFI_LOGE(LOG_TAG, "Failed to set suppliant debug level");
  }
  return nsIWifiResult::SUCCESS;
}

Result_t SupplicantStaManager::SetConcurrencyPriority(bool aEnable) {
  MutexAutoLock lock(sLock);
  IfaceType type = (aEnable ? IfaceType::STA : IfaceType::P2P);
  Status status;
  status = mSupplicant->setConcurrencyPriority(type);
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::SetPowerSave(bool aEnable) {
  MutexAutoLock lock(sLock);
  Status status;
  status = mSupplicantStaIface->setPowerSave(aEnable);
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::SetSuspendMode(bool aEnable) {
  MutexAutoLock lock(sLock);
  Status status;
  status = mSupplicantStaIface->setSuspendModeEnabled(aEnable);
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::SetExternalSim(bool aEnable) {
  MutexAutoLock lock(sLock);
  Status status;
  status = mSupplicantStaIface->setExternalSim(aEnable);
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::SetAutoReconnect(bool aEnable) {
  MutexAutoLock lock(sLock);
  Status status;
  status = mSupplicantStaIface->enableAutoReconnect(aEnable);
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::SetCountryCode(const std::string& aCountryCode) {
  MutexAutoLock lock(sLock);
  if (aCountryCode.length() != 2) {
    WIFI_LOGE(LOG_TAG, "Invalid country code: %s", aCountryCode.c_str());
    return nsIWifiResult::ERROR_INVALID_ARGS;
  }
  const std::vector<uint8_t> countryCode = {aCountryCode.at(0),
                                            aCountryCode.at(1)};
  Status status;
  status = mSupplicantStaIface->setCountryCode(countryCode);
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::SetBtCoexistenceMode(uint8_t aMode) {
  MutexAutoLock lock(sLock);
  Status status;
  status = mSupplicantStaIface->setBtCoexistenceMode((BtCoexistenceMode)aMode);
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::SetBtCoexistenceScanMode(bool aEnable) {
  MutexAutoLock lock(sLock);
  Status status;
  status = mSupplicantStaIface->setBtCoexistenceScanModeEnabled(aEnable);
  return CHECK_SUCCESS(status.isOk());
}

// Helper function to find any iface of the desired type exposed.
Result_t SupplicantStaManager::FindIfaceOfType(IfaceType aDesired,
                                               IfaceInfo* aInfo) {
  if (mSupplicant == nullptr) {
    return nsIWifiResult::ERROR_INVALID_INTERFACE;
  }

  Status status;
  std::vector<IfaceInfo> iface_infos;
  status = mSupplicant->listInterfaces(&iface_infos);
  if (!status.isOk()) {
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }

  for (const auto& info : iface_infos) {
    if (info.type == aDesired) {
      *aInfo = info;
      return nsIWifiResult::SUCCESS;
    }
  }
  return nsIWifiResult::ERROR_COMMAND_FAILED;
}

Result_t SupplicantStaManager::SetupStaInterface(
    const std::string& aInterfaceName) {
  MutexAutoLock lock(sLock);
  mInterfaceName = aInterfaceName;

  if (!mSupplicantStaIface) {
    mSupplicantStaIface = GetSupplicantStaIface();
  }

  if (!mSupplicantStaIface) {
    WIFI_LOGE(LOG_TAG, "Failed to create STA interface in supplicant");
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }

  // Instantiate supplicant callback
  android::sp<SupplicantStaIfaceCallback> supplicantCallback =
      new SupplicantStaIfaceCallback(mInterfaceName, mCallback, this);

  mSupplicantStaIface->registerCallback(supplicantCallback);
  mSupplicantStaIfaceCallback = supplicantCallback;

  return CHECK_SUCCESS(mSupplicantStaIface != nullptr &&
                       mSupplicantStaIfaceCallback != nullptr);
}

android::sp<ISupplicantStaIface> SupplicantStaManager::GetSupplicantStaIface() {
  if (mSupplicant == nullptr) {
    return nullptr;
  }
  Status status;
  android::sp<ISupplicantStaIface> sta_iface;
  status = mSupplicant->addStaInterface(getStaIfaceName(), &sta_iface);
  if (!status.isOk()) {
    ALOGE("Failed to create network\n");
    return nullptr;
  } else {
    ALOGE("Create network success\n");
  }

  return sta_iface;
}

android::sp<SupplicantStaNetwork> SupplicantStaManager::CreateStaNetwork() {
  MutexAutoLock lock(sLock);
  if (mSupplicantStaIface == nullptr) {
    return nullptr;
  }
  Status status;
  android::sp<ISupplicantStaNetwork> staNetwork;
  status = mSupplicantStaIface->addNetwork(&staNetwork);
  if (!status.isOk()) {
    WIFI_LOGE(LOG_TAG, "Failed to add network");
    return nullptr;
  }
  return new SupplicantStaNetwork(mInterfaceName, mCallback, staNetwork);
}

android::sp<SupplicantStaNetwork> SupplicantStaManager::GetStaNetwork(
    uint32_t aNetId) const {
  if (mSupplicantStaIface == nullptr) {
    return nullptr;
  }
  Status status;
  android::sp<ISupplicantStaNetwork> staNetwork;
  status = mSupplicantStaIface->getNetwork(aNetId, &staNetwork);
  if (!status.isOk()) {
    WIFI_LOGE(LOG_TAG, "Failed to get network");
    return nullptr;
  }
  return new SupplicantStaNetwork(mInterfaceName, mCallback, staNetwork);
}

android::sp<SupplicantStaNetwork> SupplicantStaManager::GetCurrentNetwork()
    const {
  std::unordered_map<std::string,
                     android::sp<SupplicantStaNetwork>>::const_iterator found =
      mCurrentNetwork.find(mInterfaceName);
  if (found == mCurrentNetwork.end()) {
    return nullptr;
  }
  return mCurrentNetwork.at(mInterfaceName);
}

NetworkConfiguration SupplicantStaManager::GetCurrentConfiguration() const {
  MutexAutoLock lock(sHashLock);
  std::unordered_map<std::string, NetworkConfiguration>::const_iterator config =
      mCurrentConfiguration.find(mInterfaceName);
  if (config == mCurrentConfiguration.end()) {
    return mDummyNetworkConfiguration;
  }
  return mCurrentConfiguration.at(mInterfaceName);
}

void SupplicantStaManager::ModifyConfigurationHash(
    int aAction, const NetworkConfiguration& aConfig) {
  MutexAutoLock lock(sHashLock);
  switch (aAction) {
    case CLEAN_ALL:
      mCurrentConfiguration.clear();
      break;
    case ERASE_CONFIG:
      mCurrentConfiguration.erase(mInterfaceName);
      break;
    case ADD_CONFIG:
      mCurrentConfiguration.insert(std::make_pair(mInterfaceName, aConfig));
      break;
  }
}

int32_t SupplicantStaManager::GetCurrentNetworkId() const {
  return GetCurrentConfiguration().mNetworkId;
}

bool SupplicantStaManager::IsCurrentEapNetwork() {
  NetworkConfiguration current = GetCurrentConfiguration();
  return current.IsValidNetwork() ? current.IsEapNetwork() : false;
}

bool SupplicantStaManager::IsCurrentPskNetwork() {
  NetworkConfiguration current = GetCurrentConfiguration();
  return current.IsValidNetwork() ? current.IsPskNetwork() : false;
}

bool SupplicantStaManager::IsCurrentSaeNetwork() {
  NetworkConfiguration current = GetCurrentConfiguration();
  return current.IsValidNetwork() ? current.IsSaeNetwork() : false;
}

bool SupplicantStaManager::IsCurrentWepNetwork() {
  NetworkConfiguration current = GetCurrentConfiguration();
  return current.IsValidNetwork() ? current.IsWepNetwork() : false;
}

Result_t SupplicantStaManager::ConnectToNetwork(ConfigurationOptions* aConfig) {
  Result_t result = nsIWifiResult::ERROR_UNKNOWN;
  android::sp<SupplicantStaNetwork> staNetwork;
  NetworkConfiguration config(aConfig);
  NetworkConfiguration existConfig = GetCurrentConfiguration();

  if (!CompareConfiguration(existConfig, mDummyNetworkConfiguration) &&
      CompareConfiguration(existConfig, config)) {
    staNetwork = GetCurrentNetwork();
    if (staNetwork == nullptr) {
      WIFI_LOGE(LOG_TAG, "Network is not available");
      return nsIWifiResult::ERROR_COMMAND_FAILED;
    }
    if (existConfig.mBssid.compare(config.mBssid)) {
      WIFI_LOGD(LOG_TAG, "Network is already saved, but need to update BSSID.");
      result = staNetwork->UpdateBssid(config.mBssid);
      if (result != nsIWifiResult::SUCCESS) {
        WIFI_LOGE(LOG_TAG, "Failed to update BSSID.");
        return result;
      }

      // update configuration
      ModifyConfigurationHash(ERASE_CONFIG, mDummyNetworkConfiguration);
      ModifyConfigurationHash(ADD_CONFIG, config);
    } else {
      WIFI_LOGD(LOG_TAG, "Same network, do not need to create a new one");
    }
  } else {
    ModifyConfigurationHash(ERASE_CONFIG, mDummyNetworkConfiguration);
    mCurrentNetwork.erase(mInterfaceName);

    // remove all supplicant networks
    result = RemoveNetworks();
    if (result != nsIWifiResult::SUCCESS) {
      WIFI_LOGE(LOG_TAG, "Failed to remove supplicant networks");
      return result;
    }

    // create network in supplicant and set configuration
    staNetwork = CreateStaNetwork();
    if (!staNetwork) {
      WIFI_LOGE(LOG_TAG, "Failed to create STA network");
      return nsIWifiResult::ERROR_INVALID_INTERFACE;
    }

    // set network configuration into supplicant
    result = staNetwork->SetConfiguration(config);
    if (result != nsIWifiResult::SUCCESS) {
      WIFI_LOGE(LOG_TAG, "Failed to set wifi configuration");
      ModifyConfigurationHash(CLEAN_ALL, mDummyNetworkConfiguration);
      mCurrentNetwork.clear();
      return result;
    }
    ModifyConfigurationHash(ADD_CONFIG, config);
    mCurrentNetwork.insert(std::make_pair(mInterfaceName, staNetwork));
  }

  // success, start to make connection
  staNetwork->SelectNetwork();
  return nsIWifiResult::SUCCESS;
}

Result_t SupplicantStaManager::Reconnect() {
  MutexAutoLock lock(sLock);
  Status status;
  status = mSupplicantStaIface->reconnect();
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::Reassociate() {
  MutexAutoLock lock(sLock);
  Status status;
  status = mSupplicantStaIface->reassociate();
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::Disconnect() {
  MutexAutoLock lock(sLock);
  Status status;
  status = mSupplicantStaIface->disconnect();
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::EnableNetwork() {
  android::sp<SupplicantStaNetwork> network;

  network = GetCurrentNetwork();
  if (network == nullptr) {
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }
  return network->EnableNetwork();
}

Result_t SupplicantStaManager::DisableNetwork() {
  android::sp<SupplicantStaNetwork> network;

  network = GetCurrentNetwork();
  if (network == nullptr) {
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }
  return network->DisableNetwork();
}

Result_t SupplicantStaManager::GetNetwork(nsWifiResult* aResult) {
  MutexAutoLock lock(sLock);
  if (!mSupplicantStaIface) {
    return nsIWifiResult::ERROR_INVALID_INTERFACE;
  }
  Status status;
  std::vector<int32_t> netIds;
  status = mSupplicantStaIface->listNetworks(&netIds);

  if (!status.isOk()) {
    WIFI_LOGE(LOG_TAG, "Failed to query saved networks in supplicant");
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }

  // By current design, network selection logic is processed in framework,
  // so there should only be at most one network in supplicant.
  if (netIds.size() != 1) {
    WIFI_LOGE(LOG_TAG, "Should only keep one network in supplicant");
    return nsIWifiResult::ERROR_UNKNOWN;
  }

  android::sp<SupplicantStaNetwork> network = GetStaNetwork(netIds.at(0));
  if (!network) {
    WIFI_LOGE(LOG_TAG, "Failed to get network %d", netIds.at(0));
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }

  // Load network configuration from supplicant
  NetworkConfiguration config;
  network->LoadConfiguration(config);

  // Assign to nsWifiConfiguration
  RefPtr<nsWifiConfiguration> configuration = new nsWifiConfiguration();
  config.ConvertConfigurations(configuration);
  aResult->updateWifiConfiguration(configuration);
  return nsIWifiResult::SUCCESS;
}

Result_t SupplicantStaManager::RemoveNetworks() {
  MutexAutoLock lock(sLock);
  if (!mSupplicantStaIface) {
    return nsIWifiResult::ERROR_INVALID_INTERFACE;
  }

  Status status;
  std::vector<int32_t> netIds;
  status = mSupplicantStaIface->listNetworks(&netIds);

  if (!status.isOk()) {
    WIFI_LOGE(LOG_TAG, "Failed to query saved networks in supplicant");
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }

  // remove network
  for (uint32_t id : netIds) {
    status = mSupplicantStaIface->removeNetwork(id);
    if (!status.isOk()) {
      WIFI_LOGE(LOG_TAG, "Failed to remove network %d", id);
      return nsIWifiResult::ERROR_COMMAND_FAILED;
    }
  }

  ModifyConfigurationHash(ERASE_CONFIG, mDummyNetworkConfiguration);
  mCurrentNetwork.erase(mInterfaceName);
  return nsIWifiResult::SUCCESS;
}

Result_t SupplicantStaManager::RoamToNetwork(ConfigurationOptions* aConfig) {
  NetworkConfiguration config(aConfig);
  NetworkConfiguration current = GetCurrentConfiguration();

  if (config.mNetworkId == INVALID_NETWORK_ID) {
    return nsIWifiResult::ERROR_INVALID_ARGS;
  }

  if (current.mNetworkId == INVALID_NETWORK_ID ||
      config.mNetworkId != current.mNetworkId ||
      config.GetNetworkKey().compare(current.GetNetworkKey())) {
    return ConnectToNetwork(aConfig);
  }

  // now we are ready to roam to the target network.
  android::sp<SupplicantStaNetwork> network =
      mCurrentNetwork.at(mInterfaceName);

  Result_t result = network->UpdateBssid(config.mBssid);

  WIFI_LOGD(LOG_TAG, "Trying to roam to network %s[%s]", config.mSsid.c_str(),
            config.mBssid.c_str());
  return (result == nsIWifiResult::SUCCESS) ? Reassociate() : result;
}

Result_t SupplicantStaManager::SendEapSimIdentityResponse(
    SimIdentityRespDataOptions* aIdentity) {
  android::sp<SupplicantStaNetwork> network;

  network = GetCurrentNetwork();
  if (network == nullptr) {
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }
  return network->SendEapSimIdentityResponse(aIdentity);
}

Result_t SupplicantStaManager::SendEapSimGsmAuthResponse(
    const nsTArray<SimGsmAuthRespDataOptions>& aGsmAuthResp) {
  android::sp<SupplicantStaNetwork> network;

  network = GetCurrentNetwork();
  if (network == nullptr) {
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }
  return network->SendEapSimGsmAuthResponse(aGsmAuthResp);
}

Result_t SupplicantStaManager::SendEapSimGsmAuthFailure() {
  android::sp<SupplicantStaNetwork> network;

  network = GetCurrentNetwork();
  if (network == nullptr) {
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }
  return network->SendEapSimGsmAuthFailure();
}

Result_t SupplicantStaManager::SendEapSimUmtsAuthResponse(
    SimUmtsAuthRespDataOptions* aUmtsAuthResp) {
  android::sp<SupplicantStaNetwork> network;

  network = GetCurrentNetwork();
  if (network == nullptr) {
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }
  return network->SendEapSimUmtsAuthResponse(aUmtsAuthResp);
}

Result_t SupplicantStaManager::SendEapSimUmtsAutsResponse(
    SimUmtsAutsRespDataOptions* aUmtsAutsResp) {
  android::sp<SupplicantStaNetwork> network;

  network = GetCurrentNetwork();
  if (network == nullptr) {
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }
  return network->SendEapSimUmtsAutsResponse(aUmtsAutsResp);
}

Result_t SupplicantStaManager::SendEapSimUmtsAuthFailure() {
  android::sp<SupplicantStaNetwork> network;

  network = GetCurrentNetwork();
  if (network == nullptr) {
    return nsIWifiResult::ERROR_COMMAND_FAILED;
  }
  return network->SendEapSimUmtsAuthFailure();
}

Result_t SupplicantStaManager::SendAnqpRequest(
    const std::array<uint8_t, 6>& aBssid,
    const std::vector<uint32_t>& aInfoElements,
    const std::vector<uint32_t>& aHs20SubTypes) {
  MutexAutoLock lock(sLock);
  Status status;
  std::vector<uint8_t> macAddr;
  status = mSupplicantStaIface->getMacAddress(&macAddr);
  if (status.isOk()) {
    status = mSupplicantStaIface->initiateAnqpQuery(
        macAddr, (std::vector<AnqpInfoId>&)aInfoElements,
        (std::vector<Hs20AnqpSubtypes>&)aHs20SubTypes);
  }
  return CHECK_SUCCESS(status.isOk());
}

/**
 * Methods to handle WPS connection
 */
Result_t SupplicantStaManager::InitWpsDetail() {
  char value[PROPERTY_VALUE_MAX];

  property_get("ro.product.name", value, "");
  std::string deviceName(value);
  if (SetWpsDeviceName(deviceName) != nsIWifiResult::SUCCESS) {
    WIFI_LOGE(LOG_TAG, "Failed to set device name: %s", value);
  }

  property_get("ro.product.manufacturer", value, "");
  std::string manufacturer(value);
  if (SetWpsManufacturer(manufacturer) != nsIWifiResult::SUCCESS) {
    WIFI_LOGE(LOG_TAG, "Failed to set manufacturer: %s", value);
  }

  property_get("ro.product.model", value, "");
  std::string modelName(value);
  if (SetWpsModelName(modelName) != nsIWifiResult::SUCCESS) {
    WIFI_LOGE(LOG_TAG, "Failed to set model name: %s", value);
  }

  property_get("ro.product.model", value, "");
  std::string modelNumber(value);
  if (SetWpsModelNumber(modelNumber) != nsIWifiResult::SUCCESS) {
    WIFI_LOGE(LOG_TAG, "Failed to set model number: %s", value);
  }

  property_get("ro.serialno", value, "");
  std::string serialNumber(value);
  if (SetWpsSerialNumber(serialNumber) != nsIWifiResult::SUCCESS) {
    WIFI_LOGE(LOG_TAG, "Failed to set serial number: %s", value);
  }

  if (SetWpsDeviceType(WPS_DEV_TYPE_DUAL_SMARTPHONE) !=
      nsIWifiResult::SUCCESS) {
    WIFI_LOGE(LOG_TAG, "Failed to set device type");
  }

  std::string configMethods("keypad physical_display virtual_push_button");
  if (SetWpsConfigMethods(configMethods) != nsIWifiResult::SUCCESS) {
    WIFI_LOGE(LOG_TAG, "Failed to set WPS config methods");
  }
  return nsIWifiResult::SUCCESS;
}

Result_t SupplicantStaManager::StartWpsRegistrar(const std::string& aBssid,
                                                 const std::string& aPinCode) {
  MutexAutoLock lock(sLock);
  std::array<uint8_t, 6> bssid;

  if (aBssid.empty() || aBssid.compare(ANY_MAC_STR) == 0) {
    bssid.fill(0);
  } else {
    ConvertMacToByteArray(aBssid, bssid);
  }
  std::vector<uint8_t> bssidVector(bssid.begin(), bssid.end());

  android::String16 pin(aPinCode.c_str(),
                        static_cast<size_t>(aPinCode.length()));

  Status status;
  status = mSupplicantStaIface->startWpsRegistrar(bssidVector, pin);
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::StartWpsPbc(const std::string& aBssid) {
  MutexAutoLock lock(sLock);
  std::array<uint8_t, 6> bssid;

  if (aBssid.empty() || aBssid.compare(ANY_MAC_STR) == 0) {
    bssid.fill(0);
  } else {
    ConvertMacToByteArray(aBssid, bssid);
  }
  std::vector<uint8_t> bssidVector(bssid.begin(), bssid.end());

  Status status;
  status = mSupplicantStaIface->startWpsPbc(bssidVector);
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::StartWpsPinKeypad(const std::string& aPinCode) {
  MutexAutoLock lock(sLock);
  android::String16 pin(aPinCode.c_str(),
                        static_cast<size_t>(aPinCode.length()));

  Status status;
  status = mSupplicantStaIface->startWpsPinKeypad(pin);
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::StartWpsPinDisplay(const std::string& aBssid,
                                                  nsAString& aGeneratedPin) {
  MutexAutoLock lock(sLock);
  std::array<uint8_t, 6> bssid;

  if (aBssid.empty() || aBssid.compare(ANY_MAC_STR) == 0) {
    bssid.fill(0);
  } else {
    ConvertMacToByteArray(aBssid, bssid);
  }

  std::vector<uint8_t> bssidVector(bssid.begin(), bssid.end());
  android::String16 pin;

  Status status;
  status = mSupplicantStaIface->startWpsPinDisplay(bssidVector, &pin);
  if (status.isOk()) {
    const char16_t* utf16Str = pin.string();
    nsDependentString pinString(utf16Str);
    aGeneratedPin.Assign(pinString);
  }
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::CancelWps() {
  MutexAutoLock lock(sLock);
  Status status;
  status = mSupplicantStaIface->cancelWps();
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::SetWpsDeviceName(
    const std::string& aDeviceName) {
  MutexAutoLock lock(sLock);
  android::String16 deviceName(aDeviceName.c_str(),
                               static_cast<size_t>(aDeviceName.length()));
  Status status;
  status = mSupplicantStaIface->setWpsDeviceName(deviceName);
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::SetWpsDeviceType(
    const std::string& aDeviceType) {
  MutexAutoLock lock(sLock);
  std::vector<std::string> tokens;
  std::string token;
  std::stringstream stream(aDeviceType);
  while (std::getline(stream, token, '-')) {
    tokens.push_back(token);
  }

  std::array<uint8_t, 2> category;
  std::array<uint8_t, 4> oui;
  std::array<uint8_t, 2> subCategory;

  uint16_t cat = std::stoi(tokens.at(0));
  category.at(0) = (cat >> 8) & 0xFF;
  category.at(1) = (cat >> 0) & 0xFF;

  ConvertHexStringToByteArray(tokens.at(1), oui);

  uint16_t subCat = std::stoi(tokens.at(2));
  subCategory.at(0) = (subCat >> 8) & 0xFF;
  subCategory.at(1) = (subCat >> 0) & 0xFF;

  std::array<uint8_t, 8> type;
  std::copy(category.cbegin(), category.cend(), type.begin());
  std::copy(oui.cbegin(), oui.cend(), type.begin() + 2);
  std::copy(subCategory.cbegin(), subCategory.cend(), type.begin() + 6);
  std::vector<uint8_t> typeVector(type.begin(), type.end());

  Status status;
  status = mSupplicantStaIface->setWpsDeviceType(typeVector);
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::SetWpsManufacturer(
    const std::string& aManufacturer) {
  MutexAutoLock lock(sLock);
  android::String16 manufacturer(aManufacturer.c_str(),
                                 static_cast<size_t>(aManufacturer.length()));
  Status status;
  status = mSupplicantStaIface->setWpsManufacturer(manufacturer);
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::SetWpsModelName(const std::string& aModelName) {
  MutexAutoLock lock(sLock);
  android::String16 modelName(aModelName.c_str(),
                              static_cast<size_t>(aModelName.length()));
  Status status;
  status = mSupplicantStaIface->setWpsManufacturer(modelName);
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::SetWpsModelNumber(
    const std::string& aModelNumber) {
  MutexAutoLock lock(sLock);
  android::String16 modelNumber(aModelNumber.c_str(),
                                static_cast<size_t>(aModelNumber.length()));
  Status status;
  status = mSupplicantStaIface->setWpsModelNumber(modelNumber);
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::SetWpsSerialNumber(
    const std::string& aSerialNumber) {
  MutexAutoLock lock(sLock);
  android::String16 serialNumber(aSerialNumber.c_str(),
                                 static_cast<size_t>(aSerialNumber.length()));
  Status status;
  status = mSupplicantStaIface->setWpsSerialNumber(serialNumber);
  return CHECK_SUCCESS(status.isOk());
}

Result_t SupplicantStaManager::SetWpsConfigMethods(
    const std::string& aConfigMethods) {
  MutexAutoLock lock(sLock);
  // TODO: The HIDL interface was using bitfield as arugment but AIDL is
  // WpsConfigMethods. It seems we need to set configs method serverl times.
  // Let's confirm with it later.
  return CHECK_SUCCESS(false);
}

int16_t SupplicantStaManager::ConvertToWpsConfigMethod(
    const std::string& aConfigMethod) {
  int16_t mask = 0;
  std::string method;
  std::stringstream stream(aConfigMethod);
  while (std::getline(stream, method, ' ')) {
    if (method.compare("usba") == 0) {
      mask |= (int16_t)WpsConfigMethods::USBA;
    } else if (method.compare("ethernet") == 0) {
      mask |= (int16_t)WpsConfigMethods::ETHERNET;
    } else if (method.compare("label") == 0) {
      mask |= (int16_t)WpsConfigMethods::LABEL;
    } else if (method.compare("display") == 0) {
      mask |= (int16_t)WpsConfigMethods::DISPLAY;
    } else if (method.compare("int_nfc_token") == 0) {
      mask |= (int16_t)WpsConfigMethods::INT_NFC_TOKEN;
    } else if (method.compare("ext_nfc_token") == 0) {
      mask |= (int16_t)WpsConfigMethods::EXT_NFC_TOKEN;
    } else if (method.compare("nfc_interface") == 0) {
      mask |= (int16_t)WpsConfigMethods::NFC_INTERFACE;
    } else if (method.compare("push_button") == 0) {
      mask |= (int16_t)WpsConfigMethods::PUSHBUTTON;
    } else if (method.compare("keypad") == 0) {
      mask |= (int16_t)WpsConfigMethods::KEYPAD;
    } else if (method.compare("virtual_push_button") == 0) {
      mask |= (int16_t)WpsConfigMethods::VIRT_PUSHBUTTON;
    } else if (method.compare("physical_push_button") == 0) {
      mask |= (int16_t)WpsConfigMethods::PHY_PUSHBUTTON;
    } else if (method.compare("p2ps") == 0) {
      mask |= (int16_t)WpsConfigMethods::P2PS;
    } else if (method.compare("virtual_display") == 0) {
      mask |= (int16_t)WpsConfigMethods::VIRT_DISPLAY;
    } else if (method.compare("physical_display") == 0) {
      mask |= (int16_t)WpsConfigMethods::PHY_DISPLAY;
    } else {
      WIFI_LOGE(LOG_TAG, "Unknown wps config method: %s", method.c_str());
    }
  }
  return mask;
}

android::String16 getP2pIfaceName() {
  std::array<char, PROPERTY_VALUE_MAX> buffer;
  property_get("wifi.direct.interface", buffer.data(), "p2p0");
  return android::String16(buffer.data());
}

/**
 * P2P functions
 */
android::sp<ISupplicantP2pIface> SupplicantStaManager::GetSupplicantP2pIface() {
  if (mSupplicant == nullptr) {
    return nullptr;
  }

  IfaceInfo info;
  if (FindIfaceOfType(IfaceType::P2P, &info) != nsIWifiResult::SUCCESS) {
    return nullptr;
  }

  Status status;
  android::sp<ISupplicantP2pIface> p2p_iface;
  status = mSupplicant->addP2pInterface(getP2pIfaceName(), &p2p_iface);
  if (!status.isOk()) {
    return nullptr;
  }
  return p2p_iface;
}

Result_t SupplicantStaManager::SetupP2pInterface() {
  MutexAutoLock lock(sLock);
  android::sp<ISupplicantP2pIface> p2p_iface = GetSupplicantP2pIface();
  if (!p2p_iface) {
    return nsIWifiResult::ERROR_INVALID_INTERFACE;
  }

  Status status;
  status = p2p_iface->saveConfig();
  if (!status.isOk()) {
    WIFI_LOGD(LOG_TAG, "[P2P] save config fail");
    // TODO: Should we return error here?
  }
  return nsIWifiResult::SUCCESS;
}

void SupplicantStaManager::RegisterDeathHandler(
    SupplicantDeathEventHandler* aHandler) {
  mDeathEventHandler = aHandler;
}

void SupplicantStaManager::UnregisterDeathHandler() {
  mDeathEventHandler = nullptr;
}

void SupplicantStaManager::SupplicantServiceDiedHandler(int32_t aCookie) {
  if (mDeathRecipientCookie != aCookie) {
    WIFI_LOGD(LOG_TAG, "Ignoring stale death recipient notification");
    return;
  }

  // TODO: notify supplicant has died.
  if (mDeathEventHandler != nullptr) {
    mDeathEventHandler->OnDeath();
  }
}

bool SupplicantStaManager::CompareConfiguration(
    const NetworkConfiguration& aOld, const NetworkConfiguration& aNew) {
  if (aOld.mNetworkId != aNew.mNetworkId) {
    return false;
  }
  if (aOld.mSsid.compare(aNew.mSsid)) {
    return false;
  }
  if (!CompareCredential(aOld, aNew)) {
    return false;
  }

  return true;
}

bool SupplicantStaManager::CompareCredential(const NetworkConfiguration& aOld,
                                             const NetworkConfiguration& aNew) {
  if (aOld.mKeyMgmt.compare(aNew.mKeyMgmt)) {
    return false;
  }
  if (aOld.mPsk.compare(aNew.mPsk)) {
    return false;
  }
  // TODO: complete the rest credentials comparation.

  return true;
}

/**
 * Hal wrapper functions
 */
android::sp<IServiceManager> SupplicantStaManager::GetServiceManager() {
  return mServiceManager ? mServiceManager : IServiceManager::getService();
}

android::sp<ISupplicant> SupplicantStaManager::GetSupplicant() {
  // TODO: If there is no mSupplicant InitSupplicantInterface then.
  return mSupplicant ? mSupplicant : nullptr;
}

bool SupplicantStaManager::SupplicantVersionSupported(const std::string& name) {
  if (!mServiceManager) {
    return false;
  }

  return mServiceManager->getTransport(name, HAL_INSTANCE_NAME) !=
         IServiceManager::Transport::EMPTY;
}

/**
 * Helper functions to notify event callback for ISupplicantCallback.
 */
void SupplicantStaManager::NotifyTerminating() {
  nsCString iface(mInterfaceName);
  RefPtr<nsWifiEvent> event = new nsWifiEvent(EVENT_SUPPLICANT_TERMINATING);

  INVOKE_CALLBACK(mCallback, event, iface);
}

/**
 * ISupplicantCallback implementation
 */
::android::binder::Status SupplicantStaManager::onInterfaceCreated(
    const ::android::String16& ifaceName) {
  WIFI_LOGD(LOG_TAG, "SupplicantCallback.onInterfaceCreated(): %s",
            ::android::String8(ifaceName).string());
  return ::android::binder::Status::fromStatusT(::android::OK);
}

::android::binder::Status SupplicantStaManager::onInterfaceRemoved(
    const ::android::String16& ifaceName) {
  WIFI_LOGD(LOG_TAG, "SupplicantCallback.onInterfaceRemoved(): %s",
            ::android::String8(ifaceName).string());
  return ::android::binder::Status::fromStatusT(::android::OK);
}
