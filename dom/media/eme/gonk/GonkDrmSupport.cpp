/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GonkDrmSupport.h"

#include "GonkDrmCDMCallbackProxy.h"
#include "GonkDrmSessionInfo.h"
#include "GonkDrmSharedData.h"
#include "GonkDrmUtils.h"
#include "json/json.h"
#include "mozilla/EMEUtils.h"
#include "nsISerialEventTarget.h"

#include <binder/ProcessState.h>
#include <media/stagefright/MediaErrors.h>
#include <mediadrm/IDrm.h>

namespace android {

using mozilla::dom::MediaKeyStatus;
using mozilla::dom::Optional;

enum class GonkDrmKeyStatus : int32_t {
  USABLE = 0,
  EXPIRED = 1,
  OUTPUT_NOT_ALLOWED = 2,
  PENDING = 3,
  INTERNAL_ERROR = 4,
  USABLE_IN_FUTURE = 5
};

static MediaKeyStatus ConvertToMediaKeyStatus(GonkDrmKeyStatus aKeyStatus) {
  switch (aKeyStatus) {
    case GonkDrmKeyStatus::USABLE:
      return MediaKeyStatus::Usable;
    case GonkDrmKeyStatus::EXPIRED:
      return MediaKeyStatus::Expired;
    case GonkDrmKeyStatus::OUTPUT_NOT_ALLOWED:
      return MediaKeyStatus::Output_restricted;
    case GonkDrmKeyStatus::PENDING:
      return MediaKeyStatus::Status_pending;
    case GonkDrmKeyStatus::INTERNAL_ERROR:
    case GonkDrmKeyStatus::USABLE_IN_FUTURE:
    default:
      return MediaKeyStatus::Internal_error;
  }
}

GonkDrmSupport::GonkDrmSupport(nsISerialEventTarget* aOwnerThread,
                               const nsAString& aOrigin,
                               const nsAString& aKeySystem)
    : mOwnerThread(aOwnerThread), mOrigin(aOrigin), mKeySystem(aKeySystem) {}

GonkDrmSupport::~GonkDrmSupport() { GD_ASSERT(!mDrm); }

void GonkDrmSupport::Init(uint32_t aPromiseId,
                          GonkDrmCDMCallbackProxy* aCallback,
                          GonkDrmStorageProxy* aStorage,
                          const sp<GonkDrmSharedData>& aSharedData) {
  GD_ASSERT(aCallback);
  GD_LOGD("%p GonkDrmSupport::Init, %s", this,
          NS_ConvertUTF16toUTF8(mKeySystem).Data());

  ProcessState::self()->startThreadPool();

  mInitPromiseId = aPromiseId;
  mCallback = aCallback;
  mStorage = aStorage;
  mSharedData = aSharedData;
  mDrm = GonkDrmUtils::MakeDrm(mKeySystem);
  if (!mDrm) {
    GD_LOGE("%p GonkDrmSupport::Init, MakeDrm failed", this);
    InitFailed();
    return;
  }

  auto err = mDrm->setListener(this);
  if (err != OK) {
    GD_LOGE("%p GonkDrmSupport::Init, DRM setListener failed(%d)", this, err);
    InitFailed();
    return;
  }

  if (mozilla::IsWidevineKeySystem(mKeySystem)) {
    // Set security level to L3.
    err = mDrm->setPropertyString(String8("securityLevel"), String8("L3"));
    if (err != OK) {
      GD_LOGW("%p GonkDrmSupport::Init, DRM set securityLevel failed(%d)", this,
              err);
    }
    // Enable session sharing.
    err = mDrm->setPropertyString(String8("sessionSharing"), String8("enable"));
    if (err != OK) {
      GD_LOGW("%p GonkDrmSupport::Init, DRM set sessionSharing failed(%d)",
              this, err);
    }
    // Enable privacy mode.
    err = mDrm->setPropertyString(String8("privacyMode"), String8("enable"));
    if (err != OK) {
      GD_LOGW("%p GonkDrmSupport::Init, DRM set privacyMode failed(%d)", this,
              err);
    }
    // Set security origin.
    auto origin = GonkDrmConverter::ToString8(NS_ConvertUTF16toUTF8(mOrigin));
    err = mDrm->setPropertyString(String8("origin"), origin);
    if (err != OK) {
      GD_LOGW("%p GonkDrmSupport::Init, DRM set origin failed(%d)", this, err);
    }
  }

  err = OpenCryptoSession();
  if (err == ERROR_DRM_NOT_PROVISIONED) {
    StartProvisioning();
    return;
  } else if (err != OK) {
    GD_LOGE("%p GonkDrmSupport::Init, OpenCryptoSession failed", this);
    InitFailed();
    return;
  }

  InitCompleted();
}

void GonkDrmSupport::InitFailed() {
  GD_ASSERT(mInitPromiseId);
  GD_ASSERT(mCallback);

  GD_LOGE("%p GonkDrmSupport::InitFailed", this);
  mCallback->RejectPromiseWithStateError(mInitPromiseId, "Init failed"_ns);
  Reset();
}

void GonkDrmSupport::InitCompleted() {
  GD_ASSERT(mInitPromiseId);
  GD_ASSERT(mCallback);

  GD_LOGD("%p GonkDrmSupport::InitCompleted", this);
  mCallback->CDMCreated(mInitPromiseId);
  mInitPromiseId = 0;
}

status_t GonkDrmSupport::OpenCryptoSession() {
  status_t err;
  auto session =
      OpenDrmSession(MediaKeySessionType::Temporary, nsCString(), &err);
  if (!session) {
    GD_LOGE("%p GonkDrmSupport::OpenCryptoSession, OpenDrmSession failed",
            this);
    return err;
  }
  mSharedData->SetCryptoSessionId(session->DrmId());
  return OK;
}

sp<GonkDrmSessionInfo> GonkDrmSupport::OpenDrmSession(
    MediaKeySessionType aSessionType, const nsCString& aEmeSessionId,
    status_t* aErr) {
  Vector<uint8_t> sessionId;
  auto err = mDrm->openSession(DrmPlugin::kSecurityLevelMax, sessionId);
  if (err != OK) {
    GD_LOGE("%p GonkDrmSupport::OpenDrmSession, DRM openSession failed(%d)",
            this, err);
    if (aErr) *aErr = err;
    return nullptr;
  }

  auto session = aSessionType == MediaKeySessionType::Temporary
                     ? GonkDrmSessionInfo::CreateTemporary(mStorage, sessionId)
                     : GonkDrmSessionInfo::CreatePersistent(mStorage, sessionId,
                                                            aEmeSessionId);
  if (!session) {
    GD_LOGE("%p GonkDrmSupport::OpenDrmSession, failed to create session info",
            this);
    if (aErr) *aErr = UNKNOWN_ERROR;
    return nullptr;
  }

  mSessionManager.Add(session);
  if (aErr) *aErr = OK;
  return session;
}

status_t GonkDrmSupport::CloseDrmSession(
    const sp<GonkDrmSessionInfo>& aSession) {
  if (!aSession) {
    return OK;
  }

  auto err = mDrm->closeSession(aSession->DrmId());
  if (err != OK) {
    GD_LOGE("%p GonkDrmSupport::CloseDrmSession, DRM closeSession failed(%d)",
            this, err);
    return err;
  }
  mSessionManager.Remove(aSession);
  return OK;
}

void GonkDrmSupport::StartProvisioning() {
  GD_LOGD("%p GonkDrmSupport::StartProvisioning", this);

  Vector<uint8_t> request;
  String8 url;
  auto err =
      mDrm->getProvisionRequest(String8("none"), String8(), request, url);
  if (err != OK) {
    GD_LOGE(
        "%p GonkDrmSupport::StartProvisioning, DRM getProvisionRequest "
        "failed(%d)",
        this, err);
    InitFailed();
    return;
  }

  GonkDrmUtils::StartProvisioning(
      GonkDrmConverter::ToNsCString(url),
      GonkDrmConverter::ToNsCString(request),
      [self = Self()](bool aSuccess, const nsACString& aResponse) {
        self->UpdateProvisioningResponse(aSuccess, aResponse);
      });
}

void GonkDrmSupport::UpdateProvisioningResponse(bool aSuccess,
                                                const nsACString& aResponse) {
  GD_LOGD("%p GonkDrmSupport::UpdateProvisioningResponse %s", this,
          aSuccess ? "succeeded" : "failed");

  if (!aSuccess) {
    InitFailed();
    return;
  }

  Vector<uint8_t> certificate, wrappedKey;
  auto err = mDrm->provideProvisionResponse(
      GonkDrmConverter::ToByteVector(aResponse), certificate, wrappedKey);
  if (err != OK) {
    GD_LOGE(
        "%p GonkDrmSupport::UpdateProvisioningResponse, DRM "
        "provideProvisionResponse failed(%d)",
        this, err);
    InitFailed();
    return;
  }

  err = OpenCryptoSession();
  if (err != OK) {
    GD_LOGE(
        "%p GonkDrmSupport::UpdateProvisioningResponse, OpenCryptoSession "
        "failed",
        this);
    InitFailed();
    return;
  }

  InitCompleted();
}

void GonkDrmSupport::Reset() {
  if (mDrm) {
    for (const auto& session : mSessionManager.All()) {
      CloseDrmSession(session);
    }
    mDrm->destroyPlugin();
    mDrm = nullptr;
  }
  if (mSharedData) {
    mSharedData->SetCryptoSessionId(Vector<uint8_t>());
    mSharedData = nullptr;
  }
  mSessionManager.Clear();
  mInitPromiseId = 0;
  mCallback = nullptr;
  mStorage = nullptr;
}

void GonkDrmSupport::Shutdown() {
  GD_LOGD("%p GonkDrmSupport::Shutdown", this);
  Reset();
}

void GonkDrmSupport::CreateSession(uint32_t aPromiseId,
                                   uint32_t aCreateSessionToken,
                                   const nsCString& aInitDataType,
                                   const nsTArray<uint8_t>& aInitData,
                                   MediaKeySessionType aSessionType) {
  GD_ASSERT(mDrm);
  GD_LOGD(
      "%p GonkDrmSupport::CreateSession, init data type %s, session type %d",
      this, aInitDataType.Data(), aSessionType);

  auto session = OpenDrmSession(aSessionType);
  if (!session) {
    GD_LOGE("%p GonkDrmSupport::CreateSession, OpenDrmSession failed", this);
    mCallback->RejectPromiseWithStateError(aPromiseId,
                                           "OpenDrmSession failed"_ns);
    return;
  }

  session->SetMimeType(aInitDataType);

  KeyRequest request;
  if (!GetKeyRequest(session, aInitData, &request)) {
    GD_LOGE("%p GonkDrmSupport::CreateSession, GetKeyRequest failed", this);
    CloseDrmSession(session);
    mCallback->RejectPromiseWithStateError(aPromiseId,
                                           "GetKeyRequest failed"_ns);
  }

  mCallback->SetSessionId(aCreateSessionToken, session->EmeId());
  mCallback->ResolvePromise(aPromiseId);
  SendKeyRequest(session, std::move(request));
  GD_LOGD("%p GonkDrmSupport::CreateSession, session opened: %s", this,
          session->EmeId().Data());
}

bool GonkDrmSupport::GetKeyRequest(const sp<GonkDrmSessionInfo>& aSession,
                                   const nsTArray<uint8_t>& aInitData,
                                   KeyRequest* aRequest) {
  auto keyType = aSession->IsReleased()    ? DrmPlugin::kKeyType_Release
                 : aSession->IsTemporary() ? DrmPlugin::kKeyType_Streaming
                                           : DrmPlugin::kKeyType_Offline;

  auto id = aSession->IsReleased() ? aSession->KeySetId() : aSession->DrmId();

  KeyedVector<String8, String8> optionalParameters;
  Vector<uint8_t> request;
  String8 defaultUrl;
  DrmPlugin::KeyRequestType keyRequestType;

  auto err = mDrm->getKeyRequest(
      id, GonkDrmConverter::ToByteVector(aInitData),
      GonkDrmConverter::ToString8(aSession->MimeType()), keyType,
      optionalParameters, request, defaultUrl, &keyRequestType);
  if (err != OK) {
    GD_LOGE("%p GonkDrmSupport::GetKeyRequest, DRM getKeyRequest failed(%d)",
            this, err);
    return false;
  }

  MediaKeyMessageType messageType;

  switch (keyRequestType) {
    case DrmPlugin::kKeyRequestType_Initial:
      messageType = MediaKeyMessageType::License_request;
      break;
    case DrmPlugin::kKeyRequestType_Renewal:
      messageType = MediaKeyMessageType::License_renewal;
      break;
    case DrmPlugin::kKeyRequestType_Release:
      messageType = MediaKeyMessageType::License_release;
      break;
    default:
      GD_LOGE(
          "%p GonkDrmSupport::GetKeyRequest, unsupported key request type %d",
          this, keyRequestType);
      return false;
  }

  aRequest->first = messageType;
  aRequest->second = GonkDrmConverter::ToNsByteArray(request);
  return true;
}

void GonkDrmSupport::SendKeyRequest(const sp<GonkDrmSessionInfo>& aSession,
                                    KeyRequest&& aRequest) {
  mCallback->SessionMessage(aSession->EmeId(), aRequest.first,
                            std::move(aRequest.second));
}

void GonkDrmSupport::LoadSession(uint32_t aPromiseId,
                                 const nsCString& aEmeSessionId) {
  GD_ASSERT(mDrm);
  GD_LOGD("%p GonkDrmSupport::LoadSession, session ID %s", this,
          aEmeSessionId.Data());

  auto session =
      OpenDrmSession(MediaKeySessionType::Persistent_license, aEmeSessionId);

  auto successCb = [aPromiseId, this, self = Self()]() {
    GD_LOGD("%p GonkDrmSupport::LoadSession succeeded", this);
    mCallback->ResolveLoadSessionPromise(aPromiseId, true);
  };

  auto failureCb = [aPromiseId, session, this,
                    self = Self()](const nsACString& aReason) {
    GD_LOGE("%p GonkDrmSupport::LoadSession, %s", this, aReason.Data());
    CloseDrmSession(session);
    mCallback->RejectPromiseWithStateError(aPromiseId, nsCString(aReason));
  };

  LoadSession(session, successCb, failureCb);
}

void GonkDrmSupport::LoadSession(const sp<GonkDrmSessionInfo>& aSession,
                                 SuccessCallback aSuccessCb,
                                 FailureCallback aFailureCb) {
  if (!aSession) {
    aFailureCb("session not found"_ns);
    return;
  }

  aSession->LoadFromStorage(
      [aSession, aSuccessCb, aFailureCb, this, self = Self()]() {
        // If the session was marked as released in RemoveSession() but somehow
        // we didn't receive the server response through UpdateSession(), we
        // should avoid restoring the key and just report success to let JS
        // release it again.
        if (aSession->IsReleased()) {
          GD_LOGD("%p GonkDrmSupport::LoadSession, session is released", this);
          aSuccessCb();

          // Report expiration with dummy key ID to JS.
          auto status = Optional<MediaKeyStatus>(MediaKeyStatus::Expired);
          NotifyKeyStatus(aSession, {CDMKeyInfo(mDummyKeyId, status)});
          return;
        }

        auto err = mDrm->restoreKeys(aSession->DrmId(), aSession->KeySetId());
        if (err != OK) {
          aFailureCb(nsPrintfCString("DRM restoreKeys failed(%d)", err));
          return;
        }

        aSuccessCb();
      },
      aFailureCb);
}

void GonkDrmSupport::UpdateSession(uint32_t aPromiseId,
                                   const nsCString& aEmeSessionId,
                                   const nsTArray<uint8_t>& aResponse) {
  GD_ASSERT(mDrm);
  GD_LOGD("%p GonkDrmSupport::UpdateSession, session ID %s", this,
          aEmeSessionId.Data());

  auto session = mSessionManager.FindByEmeId(aEmeSessionId);

  auto successCb = [aPromiseId, this, self = Self()]() {
    GD_LOGD("%p GonkDrmSupport::UpdateSession succeeded", this);
    mCallback->ResolvePromise(aPromiseId);
  };

  auto failureCb = [aPromiseId, session, this,
                    self = Self()](const nsACString& aReason) {
    GD_LOGE("%p GonkDrmSupport::UpdateSession, %s", this, aReason.Data());
    mCallback->RejectPromiseWithStateError(aPromiseId, nsCString(aReason));
    if (session) {
      mCallback->SessionError(session->EmeId(), NS_ERROR_DOM_INVALID_STATE_ERR,
                              -1, nsCString(aReason));
    }
  };

  UpdateSession(session, aResponse, successCb, failureCb);
}

void GonkDrmSupport::UpdateSession(const sp<GonkDrmSessionInfo>& aSession,
                                   const nsTArray<uint8_t>& aResponse,
                                   SuccessCallback aSuccessCb,
                                   FailureCallback aFailureCb) {
  if (!aSession) {
    aFailureCb("session not found"_ns);
    return;
  }

  auto id = aSession->IsReleased() ? aSession->KeySetId() : aSession->DrmId();
  auto response = GonkDrmConverter::ToByteVector(aResponse);
  Vector<uint8_t> keySetId;

  auto err = mDrm->provideKeyResponse(id, response, keySetId);
  if (err != OK) {
    aFailureCb(nsPrintfCString("DRM provideKeyResponse failed(%d)", err));
    return;
  }

#ifdef GONK_DRM_PEEK_CLEARKEY_KEY_STATUS
  if (mozilla::IsClearkeyKeySystem(mKeySystem)) {
    PeekClearkeyKeyStatus(aSession, aResponse);
  }
#endif

  if (aSession->IsTemporary()) {
    // For a temporary session, we are done here.
    aSuccessCb();
  } else if (aSession->IsReleased()) {
    // For a released session, we have provided the server response to MediaDrm.
    // We can now erase the session from the storage.
    aSession->EraseFromStorage(aSuccessCb, aFailureCb);
  } else {
    // For a persistent session, we now have a key set ID. Save it to the
    // storage.
    aSession->SetKeySetId(keySetId);
    aSession->SaveToStorage(aSuccessCb, aFailureCb);
  }
}

void GonkDrmSupport::CloseSession(uint32_t aPromiseId,
                                  const nsCString& aEmeSessionId) {
  GD_ASSERT(mDrm);
  GD_LOGD("%p GonkDrmSupport::CloseSession, session ID %s", this,
          aEmeSessionId.Data());

  auto session = mSessionManager.FindByEmeId(aEmeSessionId);
  if (!session) {
    GD_LOGE("%p GonkDrmSupport::CloseSession, session not found", this);
    mCallback->RejectPromiseWithStateError(aPromiseId, "session not found"_ns);
    return;
  }

  auto err = CloseDrmSession(session);
  if (err != OK) {
    GD_LOGE("%p GonkDrmSupport::CloseSession, DRM closeSession failed(%d)",
            this, err);
    mCallback->RejectPromiseWithStateError(aPromiseId,
                                           "closeSession failed"_ns);
    return;
  }

  mSharedData->RemoveSession(session->DrmId());
  mCallback->ResolvePromise(aPromiseId);
  mCallback->SessionClosed(session->EmeId());
}

void GonkDrmSupport::RemoveSession(uint32_t aPromiseId,
                                   const nsCString& aEmeSessionId) {
  GD_ASSERT(mDrm);
  GD_LOGD("%p GonkDrmSupport::RemoveSession, session ID %s", this,
          aEmeSessionId.Data());

  auto session = mSessionManager.FindByEmeId(aEmeSessionId);

  auto successCb = [aPromiseId, this, self = Self()]() {
    GD_LOGD("%p GonkDrmSupport::RemoveSession succeeded", this);
    mCallback->ResolvePromise(aPromiseId);
  };

  auto failureCb = [aPromiseId, this,
                    self = Self()](const nsACString& aReason) {
    GD_LOGE("%p GonkDrmSupport::RemoveSession, %s", this, aReason.Data());
    mCallback->RejectPromiseWithStateError(aPromiseId, nsCString(aReason));
  };

  RemoveSession(session, successCb, failureCb);
}

void GonkDrmSupport::RemoveSession(const sp<GonkDrmSessionInfo>& aSession,
                                   SuccessCallback aSuccessCb,
                                   FailureCallback aFailureCb) {
  if (!aSession) {
    aFailureCb("session not found"_ns);
    return;
  }

  if (aSession->IsTemporary()) {
    aFailureCb("session not persistent"_ns);
    return;
  }

  if (aSession->KeySetId().empty()) {
    aFailureCb("key set ID not found"_ns);
    return;
  }

  // First mark this session as released until the following steps complete:
  // 1. We have sent the key release request to the server.
  // 2. We have received the server response through UpdateSession().
  // 3. We have set the response to MediaDrm so the keys are actually released.
  // And then we will erase this session from the storage in UpdateSession().
  aSession->SetReleased();
  aSession->SaveToStorage(
      [aSuccessCb, aFailureCb, aSession, this, self = Self()]() {
        // Generate key release request.
        KeyRequest request;
        if (!GetKeyRequest(aSession, {}, &request)) {
          aFailureCb("GetKeyRequest failed"_ns);
          return;
        }
        aSuccessCb();
        SendKeyRequest(aSession, std::move(request));
      },
      aFailureCb);
}

void GonkDrmSupport::SetServerCertificate(uint32_t aPromiseId,
                                          const nsTArray<uint8_t>& aCert) {
  GD_ASSERT(mDrm);
  GD_LOGD("%p GonkDrmSupport::SetServerCertificate", this);

  auto err = mDrm->setPropertyByteArray(String8("serviceCertificate"),
                                        GonkDrmConverter::ToByteVector(aCert));
  if (err != OK) {
    GD_LOGE(
        "%p GonkDrmSupport::SetServerCertificate, DRM set serviceCertificate "
        "failed(%d)",
        this, err);
    mCallback->RejectPromiseWithStateError(aPromiseId,
                                           "set serviceCertificate failed"_ns);
    return;
  }
  mCallback->ResolvePromise(aPromiseId);
}

// Called on binder thread.
void GonkDrmSupport::notify(DrmPlugin::EventType aEventType, int aExtra,
                            const Parcel* aObj) {
  GD_LOGV("%p GonkDrmSupport::notify, event %d, extra %d, parcel %p", this,
          aEventType, aExtra, aObj);

  // Make a copy of the Parcel and dispatch it to owner thread.
  std::unique_ptr<Parcel> parcel;
  if (aObj) {
    parcel.reset(new Parcel());
    parcel->appendFrom(aObj, 0, aObj->dataSize());
    parcel->setDataPosition(0);
  }

  mOwnerThread->Dispatch(NS_NewRunnableFunction(
      "GonkDrmSupport::notify",
      [aEventType, aExtra, obj = std::move(parcel), this, self = Self()]() {
        Notify(aEventType, aExtra, obj.get());
      }));
}

void GonkDrmSupport::Notify(DrmPlugin::EventType aEventType, int aExtra,
                            const Parcel* aObj) {
  if (!mDrm) {
    GD_LOGD("%p GonkDrmSupport::Notify, already shut down", this);
    return;
  }

  switch (aEventType) {
    case DrmPlugin::kDrmPluginEventKeyNeeded:
      OnKeyNeeded(aObj);
      break;
    case DrmPlugin::kDrmPluginEventExpirationUpdate:
      OnExpirationUpdated(aObj);
      break;
    case DrmPlugin::kDrmPluginEventKeysChange:
      OnKeyStatusChanged(aObj);
      break;
  }
}

void GonkDrmSupport::OnKeyNeeded(const Parcel* aParcel) {
  if (!aParcel) {
    return;
  }

  auto sessionId = GonkDrmUtils::ReadByteVectorFromParcel(aParcel);
  auto session = mSessionManager.FindByDrmId(sessionId);
  if (!session) {
    GD_LOGE("%p GonkDrmSupport::OnKeyNeeded, session not found", this);
    return;
  }

  auto data = GonkDrmConverter::ToNsByteArray(
      GonkDrmUtils::ReadByteVectorFromParcel(aParcel));

  KeyRequest request;
  if (!GetKeyRequest(session, data, &request)) {
    GD_LOGE("%p GonkDrmSupport::OnKeyNeeded, GetKeyRequest failed", this);
    return;
  }

  GD_LOGD("%p GonkDrmSupport::OnKeyNeeded, session ID %s sending key request",
          this, session->EmeId().Data());
  SendKeyRequest(session, std::move(request));
}

void GonkDrmSupport::OnExpirationUpdated(const Parcel* aParcel) {
  if (!aParcel) {
    return;
  }

  auto sessionId = GonkDrmUtils::ReadByteVectorFromParcel(aParcel);
  auto session = mSessionManager.FindByDrmId(sessionId);
  if (!session) {
    GD_LOGE("%p GonkDrmSupport::OnExpirationUpdate, session not found", this);
    return;
  }

  auto expirationTime = aParcel->readInt64();

  GD_LOGD(
      "%p GonkDrmSupport::OnExpirationUpdate, session ID %s, expiration time "
      "%" PRIi64,
      this, session->EmeId().Data(), expirationTime);
  mCallback->ExpirationChange(session->EmeId(), expirationTime);
}

void GonkDrmSupport::OnKeyStatusChanged(const Parcel* aParcel) {
  GD_ASSERT(mCallback);

  if (!aParcel) {
    return;
  }

#ifdef GONK_DRM_PEEK_CLEARKEY_KEY_STATUS
  // MediaDrm ClearKey plugin only reports fake key status. Instead we use
  // PeekClearkeyKeyStatus() to parse actual status from the server response.
  if (mozilla::IsClearkeyKeySystem(mKeySystem)) {
    return;
  }
#endif

  nsTArray<CDMKeyInfo> keyInfos;
  auto sessionId = GonkDrmUtils::ReadByteVectorFromParcel(aParcel);
  auto session = mSessionManager.FindByDrmId(sessionId);
  if (!session) {
    GD_LOGE("%p GonkDrmSupport::OnKeyStatusChanged, session not found", this);
    return;
  }

  for (auto num = aParcel->readInt32(); num > 0; num--) {
    auto keyId = GonkDrmUtils::ReadByteVectorFromParcel(aParcel);
    auto keyStatus = static_cast<GonkDrmKeyStatus>(aParcel->readInt32());
    keyInfos.EmplaceBack(
        GonkDrmConverter::ToNsByteArray(keyId),
        Optional<MediaKeyStatus>(ConvertToMediaKeyStatus(keyStatus)));
  }

  GD_LOGD("%p GonkDrmSupport::OnKeyStatusChanged, session ID %s", this,
          session->EmeId().Data());
  NotifyKeyStatus(session, std::move(keyInfos));
}

#ifdef GONK_DRM_PEEK_CLEARKEY_KEY_STATUS
void GonkDrmSupport::PeekClearkeyKeyStatus(
    const sp<GonkDrmSessionInfo>& aSession,
    const nsTArray<uint8_t>& aResponse) {
  GD_ASSERT(mozilla::IsClearkeyKeySystem(mKeySystem));

  auto response = GonkDrmConverter::ToNsCString(aResponse);
  GD_LOGD(
      "%p GonkDrmSupport::PeekClearkeyKeyStatus, session ID %s, response %s",
      this, aSession->EmeId().Data(), response.Data());

  Json::Value root;
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(response.BeginReading(), response.EndReading(), &root,
                     nullptr)) {
    GD_LOGE("%p GonkDrmSupport::PeekClearkeyKeyStatus, parse failed", this);
    return;
  }

  nsTArray<CDMKeyInfo> keyInfos;
  for (auto& key : root["keys"]) {
    auto kid = GonkDrmConverter::ToNsCString(key["kid"].asString());
    auto keyId = GonkDrmUtils::DecodeBase64URL(kid);
    keyInfos.EmplaceBack(GonkDrmConverter::ToNsByteArray(keyId),
                         Optional<MediaKeyStatus>(MediaKeyStatus::Usable));
  }
  NotifyKeyStatus(aSession, std::move(keyInfos));
}
#endif

void GonkDrmSupport::NotifyKeyStatus(const sp<GonkDrmSessionInfo>& aSession,
                                     nsTArray<CDMKeyInfo>&& aKeyInfos) {
  GD_ASSERT(aSession);

  for (const auto& info : aKeyInfos) {
    if (info.mKeyId != mDummyKeyId) {
      mSharedData->AddKey(aSession->DrmId(),
                          GonkDrmConverter::ToByteVector(info.mKeyId));
    }
  }
  mCallback->BatchedKeyStatusChanged(aSession->EmeId(), std::move(aKeyInfos));
}

void GonkDrmSupport::SessionManager::Add(
    const sp<GonkDrmSessionInfo>& aSession) {
  auto emeId = aSession->EmeId();
  auto drmId = GonkDrmConverter::ToStdByteVector(aSession->DrmId());
  mEmeSessionIdMap[emeId] = aSession;
  mDrmSessionIdMap[drmId] = aSession;
}

void GonkDrmSupport::SessionManager::Remove(
    const sp<GonkDrmSessionInfo>& aSession) {
  auto emeId = aSession->EmeId();
  auto drmId = GonkDrmConverter::ToStdByteVector(aSession->DrmId());
  mEmeSessionIdMap.erase(emeId);
  mDrmSessionIdMap.erase(drmId);
}

void GonkDrmSupport::SessionManager::Clear() {
  mEmeSessionIdMap.clear();
  mDrmSessionIdMap.clear();
}

std::vector<sp<GonkDrmSessionInfo>> GonkDrmSupport::SessionManager::All() {
  std::vector<sp<GonkDrmSessionInfo>> v;
  for (const auto& [drmId, session] : mDrmSessionIdMap) {
    v.push_back(session);
  }
  return v;
}

sp<GonkDrmSessionInfo> GonkDrmSupport::SessionManager::FindByEmeId(
    const nsCString& aEmeId) {
  if (mEmeSessionIdMap.count(aEmeId)) {
    return mEmeSessionIdMap[aEmeId];
  }
  return nullptr;
}

sp<GonkDrmSessionInfo> GonkDrmSupport::SessionManager::FindByDrmId(
    const Vector<uint8_t>& aDrmId) {
  auto drmId = GonkDrmConverter::ToStdByteVector(aDrmId);
  if (mDrmSessionIdMap.count(drmId)) {
    return mDrmSessionIdMap[drmId];
  }
  return nullptr;
}

}  // namespace android
