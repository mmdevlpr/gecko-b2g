/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SmsParent.h"

#include "nsISmsService.h"
#include "nsIMmsService.h"
#include "nsIObserverService.h"
#include "mozilla/Services.h"
#include "nsISmsMessage.h"
#include "nsIMmsMessage.h"
#include "SmsMessageInternal.h"
#include "MmsMessageInternal.h"
#include "nsIMobileMessageDatabaseService.h"
#include "MobileMessageThreadInternal.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/File.h"
#include "mozilla/dom/IPCBlobUtils.h"
#include "mozilla/dom/ToJSValue.h"
#include "mozilla/dom/mobilemessage/Constants.h"  // For MessageType
#include "mozilla/UniquePtr.h"
#include "nsContentUtils.h"
#include "xpcpublic.h"
#include "nsServiceManagerUtils.h"
#include "DeletedMessageInfo.h"

using mozilla::ipc::IPCResult;

namespace mozilla {
namespace dom {
namespace mobilemessage {

static JSObject* MmsAttachmentDataToJSObject(
    JSContext* aContext, const MmsAttachmentData& aAttachment) {
  JS::Rooted<JSObject*> obj(aContext, JS_NewPlainObject(aContext));
  NS_ENSURE_TRUE(obj, nullptr);

  JS::Rooted<JSString*> idStr(
      aContext, JS_NewUCStringCopyN(aContext, aAttachment.id().get(),
                                    aAttachment.id().Length()));
  NS_ENSURE_TRUE(idStr, nullptr);
  if (!JS_DefineProperty(aContext, obj, "id", idStr, 0)) {
    return nullptr;
  }

  JS::Rooted<JSString*> locStr(
      aContext, JS_NewUCStringCopyN(aContext, aAttachment.location().get(),
                                    aAttachment.location().Length()));
  NS_ENSURE_TRUE(locStr, nullptr);
  if (!JS_DefineProperty(aContext, obj, "location", locStr, 0)) {
    return nullptr;
  }

  RefPtr<BlobImpl> blobImpl = IPCBlobUtils::Deserialize(aAttachment.content());

  // RefPtr<File> needs to go out of scope before toObjectOrNull() is
  // called because the static analysis thinks dereferencing XPCOM objects
  // can GC (because in some cases it can!), and a return statement with a
  // JSObject* type means that JSObject* is on the stack as a raw pointer
  // while destructors are running.
  JS::Rooted<JS::Value> content(aContext);
  {
    nsIGlobalObject* global =
        xpc::NativeGlobal(JS::CurrentGlobalOrNull(aContext));
    MOZ_ASSERT(global);

    RefPtr<Blob> blob = Blob::Create(global, blobImpl);
    if (!ToJSValue(aContext, blob, &content)) {
      return nullptr;
    }
  }

  if (!JS_DefineProperty(aContext, obj, "content", content, 0)) {
    return nullptr;
  }

  return obj;
}

static bool GetParamsFromSendMmsMessageRequest(
    JSContext* aCx, const SendMmsMessageRequest& aRequest, JS::Value* aParam) {
  JS::Rooted<JSObject*> paramsObj(aCx, JS_NewPlainObject(aCx));
  NS_ENSURE_TRUE(paramsObj, false);

  // smil
  JS::Rooted<JSString*> smilStr(aCx,
                                JS_NewUCStringCopyN(aCx, aRequest.smil().get(),
                                                    aRequest.smil().Length()));
  NS_ENSURE_TRUE(smilStr, false);
  if (!JS_DefineProperty(aCx, paramsObj, "smil", smilStr, 0)) {
    return false;
  }

  // subject
  JS::Rooted<JSString*> subjectStr(
      aCx, JS_NewUCStringCopyN(aCx, aRequest.subject().get(),
                               aRequest.subject().Length()));
  NS_ENSURE_TRUE(subjectStr, false);
  if (!JS_DefineProperty(aCx, paramsObj, "subject", subjectStr, 0)) {
    return false;
  }

  // receivers
  JS::Rooted<JS::Value> receiverArray(aCx);
  if (!ToJSValue(aCx, aRequest.receivers(), &receiverArray)) {
    return false;
  }
  if (!JS_DefineProperty(aCx, paramsObj, "receivers", receiverArray, 0)) {
    return false;
  }

  // attachments
  JS::Rooted<JSObject*> attachmentArray(
      aCx, JS::NewArrayObject(aCx, aRequest.attachments().Length()));
  for (uint32_t i = 0; i < aRequest.attachments().Length(); i++) {
    JS::Rooted<JSObject*> obj(
        aCx,
        MmsAttachmentDataToJSObject(aCx, aRequest.attachments().ElementAt(i)));
    NS_ENSURE_TRUE(obj, false);
    if (!JS_DefineElement(aCx, attachmentArray, i, obj, JSPROP_ENUMERATE)) {
      return false;
    }
  }

  if (!JS_DefineProperty(aCx, paramsObj, "attachments", attachmentArray, 0)) {
    return false;
  }

  // group
  JS::Rooted<JS::Value> isGroupBool(aCx, JS::BooleanValue(aRequest.isGroup()));
  if (!JS_DefineProperty(aCx, paramsObj, "isGroup", isGroupBool, 0)) {
    return false;
  }

  aParam->setObject(*paramsObj);
  return true;
}

static bool GetMobileMessageDataFromMessage(nsISupports* aMsg,
                                            MobileMessageData& aData) {
  if (!aMsg) {
    NS_WARNING("Invalid message to convert!");
    return false;
  }

  nsCOMPtr<nsIMmsMessage> mmsMsg = do_QueryInterface(aMsg);
  if (mmsMsg) {
    MmsMessageData data;
    if (!static_cast<MmsMessageInternal*>(mmsMsg.get())
             ->GetData(data)) {
      return false;
    }
    aData = data;
    return true;
  }

  nsCOMPtr<nsISmsMessage> smsMsg = do_QueryInterface(aMsg);
  if (smsMsg) {
    aData = static_cast<SmsMessageInternal*>(smsMsg.get())->GetData();
    return true;
  }

  NS_WARNING("Cannot get MobileMessageData");
  return false;
}

NS_IMPL_ISUPPORTS(SmsParent, nsIObserver)

SmsParent::SmsParent() {
  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  if (!obs) {
    return;
  }

  obs->AddObserver(this, kSmsReceivedObserverTopic, false);
  obs->AddObserver(this, kSmsRetrievingObserverTopic, false);
  obs->AddObserver(this, kSmsSendingObserverTopic, false);
  obs->AddObserver(this, kSmsSentObserverTopic, false);
  obs->AddObserver(this, kSmsFailedObserverTopic, false);
  obs->AddObserver(this, kSmsDeliverySuccessObserverTopic, false);
  obs->AddObserver(this, kSmsDeliveryErrorObserverTopic, false);
  obs->AddObserver(this, kSilentSmsReceivedObserverTopic, false);
  obs->AddObserver(this, kSmsReadSuccessObserverTopic, false);
  obs->AddObserver(this, kSmsReadErrorObserverTopic, false);
  obs->AddObserver(this, kSmsDeletedObserverTopic, false);
}

void SmsParent::ActorDestroy(ActorDestroyReason why) {
  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  if (!obs) {
    return;
  }

  obs->RemoveObserver(this, kSmsReceivedObserverTopic);
  obs->RemoveObserver(this, kSmsRetrievingObserverTopic);
  obs->RemoveObserver(this, kSmsSendingObserverTopic);
  obs->RemoveObserver(this, kSmsSentObserverTopic);
  obs->RemoveObserver(this, kSmsFailedObserverTopic);
  obs->RemoveObserver(this, kSmsDeliverySuccessObserverTopic);
  obs->RemoveObserver(this, kSmsDeliveryErrorObserverTopic);
  obs->RemoveObserver(this, kSilentSmsReceivedObserverTopic);
  obs->RemoveObserver(this, kSmsReadSuccessObserverTopic);
  obs->RemoveObserver(this, kSmsReadErrorObserverTopic);
  obs->RemoveObserver(this, kSmsDeletedObserverTopic);
}

NS_IMETHODIMP
SmsParent::Observe(nsISupports* aSubject, const char* aTopic,
                   const char16_t* aData) {
  if (!strcmp(aTopic, kSmsReceivedObserverTopic)) {
    MobileMessageData msgData;
    if (!GetMobileMessageDataFromMessage(aSubject, msgData)) {
      NS_ERROR("Got a 'sms-received' topic without a valid message!");
      return NS_OK;
    }

    Unused << SendNotifyReceivedMessage(msgData);
    return NS_OK;
  }

  if (!strcmp(aTopic, kSmsRetrievingObserverTopic)) {
    MobileMessageData msgData;
    if (!GetMobileMessageDataFromMessage(aSubject, msgData)) {
      NS_ERROR("Got a 'sms-retrieving' topic without a valid message!");
      return NS_OK;
    }

    Unused << SendNotifyRetrievingMessage(msgData);
    return NS_OK;
  }

  if (!strcmp(aTopic, kSmsSendingObserverTopic)) {
    MobileMessageData msgData;
    if (!GetMobileMessageDataFromMessage(aSubject, msgData)) {
      NS_ERROR("Got a 'sms-sending' topic without a valid message!");
      return NS_OK;
    }

    Unused << SendNotifySendingMessage(msgData);
    return NS_OK;
  }

  if (!strcmp(aTopic, kSmsSentObserverTopic)) {
    MobileMessageData msgData;
    if (!GetMobileMessageDataFromMessage(aSubject, msgData)) {
      NS_ERROR("Got a 'sms-sent' topic without a valid message!");
      return NS_OK;
    }

    Unused << SendNotifySentMessage(msgData);
    return NS_OK;
  }

  if (!strcmp(aTopic, kSmsFailedObserverTopic)) {
    MobileMessageData msgData;
    if (!GetMobileMessageDataFromMessage(aSubject, msgData)) {
      NS_ERROR("Got a 'sms-failed' topic without a valid message!");
      return NS_OK;
    }

    Unused << SendNotifyFailedMessage(msgData);
    return NS_OK;
  }

  if (!strcmp(aTopic, kSmsDeliverySuccessObserverTopic)) {
    MobileMessageData msgData;
    if (!GetMobileMessageDataFromMessage(aSubject, msgData)) {
      NS_ERROR("Got a 'sms-sending' topic without a valid message!");
      return NS_OK;
    }

    Unused << SendNotifyDeliverySuccessMessage(msgData);
    return NS_OK;
  }

  if (!strcmp(aTopic, kSmsDeliveryErrorObserverTopic)) {
    MobileMessageData msgData;
    if (!GetMobileMessageDataFromMessage(aSubject, msgData)) {
      NS_ERROR("Got a 'sms-delivery-error' topic without a valid message!");
      return NS_OK;
    }

    Unused << SendNotifyDeliveryErrorMessage(msgData);
    return NS_OK;
  }

  if (!strcmp(aTopic, kSilentSmsReceivedObserverTopic)) {
    nsCOMPtr<nsISmsMessage> smsMsg = do_QueryInterface(aSubject);
    if (!smsMsg) {
      return NS_OK;
    }

    nsString sender;
    if (NS_FAILED(smsMsg->GetSender(sender)) ||
        !mSilentNumbers.Contains(sender)) {
      return NS_OK;
    }

    MobileMessageData msgData =
        static_cast<SmsMessageInternal*>(smsMsg.get())->GetData();
    Unused << SendNotifyReceivedSilentMessage(msgData);
    return NS_OK;
  }

  if (!strcmp(aTopic, kSmsReadSuccessObserverTopic)) {
    MobileMessageData msgData;
    if (!GetMobileMessageDataFromMessage(aSubject, msgData)) {
      NS_ERROR("Got a 'sms-read-success' topic without a valid message!");
      return NS_OK;
    }

    Unused << SendNotifyReadSuccessMessage(msgData);
    return NS_OK;
  }

  if (!strcmp(aTopic, kSmsReadErrorObserverTopic)) {
    MobileMessageData msgData;
    if (!GetMobileMessageDataFromMessage(aSubject, msgData)) {
      NS_ERROR("Got a 'sms-read-error' topic without a valid message!");
      return NS_OK;
    }

    Unused << SendNotifyReadErrorMessage(msgData);
    return NS_OK;
  }

  if (!strcmp(aTopic, kSmsDeletedObserverTopic)) {
    nsCOMPtr<nsIDeletedMessageInfo> deletedInfo = do_QueryInterface(aSubject);
    if (!deletedInfo) {
      NS_ERROR("Got a 'sms-deleted' topic without a valid message!");
      return NS_OK;
    }

    Unused << SendNotifyDeletedMessageInfo(
        static_cast<DeletedMessageInfo*>(deletedInfo.get())->GetData());
    return NS_OK;
  }

  return NS_OK;
}

IPCResult SmsParent::RecvAddSilentNumber(const nsString& aNumber) {
  if (mSilentNumbers.Contains(aNumber)) {
    return IPC_OK();
  }

  nsCOMPtr<nsISmsService> smsService = do_GetService(SMS_SERVICE_CONTRACTID);
  NS_ENSURE_TRUE(smsService, IPC_OK());

  nsresult rv = smsService->AddSilentNumber(aNumber);
  if (NS_SUCCEEDED(rv)) {
    mSilentNumbers.AppendElement(aNumber);
  }

  return IPC_OK();
}

IPCResult SmsParent::RecvRemoveSilentNumber(const nsString& aNumber) {
  if (!mSilentNumbers.Contains(aNumber)) {
    return IPC_OK();
  }

  nsCOMPtr<nsISmsService> smsService = do_GetService(SMS_SERVICE_CONTRACTID);
  NS_ENSURE_TRUE(smsService, IPC_OK());

  nsresult rv = smsService->RemoveSilentNumber(aNumber);
  if (NS_SUCCEEDED(rv)) {
    mSilentNumbers.RemoveElement(aNumber);
  }

  return IPC_OK();
}

IPCResult SmsParent::RecvPSmsRequestConstructor(
    PSmsRequestParent* aActor, const IPCSmsRequest& aRequest) {
  SmsRequestParent* actor = static_cast<SmsRequestParent*>(aActor);

  switch (aRequest.type()) {
    case IPCSmsRequest::TSendMessageRequest:
      return actor->DoRequest(aRequest.get_SendMessageRequest())
                 ? IPC_OK()
                 : IPC_FAIL_NO_REASON(this);
    case IPCSmsRequest::TRetrieveMessageRequest:
      return actor->DoRequest(aRequest.get_RetrieveMessageRequest())
                 ? IPC_OK()
                 : IPC_FAIL_NO_REASON(this);
    case IPCSmsRequest::TGetMessageRequest:
      return actor->DoRequest(aRequest.get_GetMessageRequest())
                 ? IPC_OK()
                 : IPC_FAIL_NO_REASON(this);
    case IPCSmsRequest::TDeleteMessageRequest:
      return actor->DoRequest(aRequest.get_DeleteMessageRequest())
                 ? IPC_OK()
                 : IPC_FAIL_NO_REASON(this);
    case IPCSmsRequest::TMarkMessageReadRequest:
      return actor->DoRequest(aRequest.get_MarkMessageReadRequest())
                 ? IPC_OK()
                 : IPC_FAIL_NO_REASON(this);
    case IPCSmsRequest::TGetSegmentInfoForTextRequest:
      return actor->DoRequest(aRequest.get_GetSegmentInfoForTextRequest())
                 ? IPC_OK()
                 : IPC_FAIL_NO_REASON(this);
    case IPCSmsRequest::TGetSmscAddressRequest:
      return actor->DoRequest(aRequest.get_GetSmscAddressRequest())
                 ? IPC_OK()
                 : IPC_FAIL_NO_REASON(this);
    case IPCSmsRequest::TSetSmscAddressRequest:
      return actor->DoRequest(aRequest.get_SetSmscAddressRequest())
                 ? IPC_OK()
                 : IPC_FAIL_NO_REASON(this);
    default:
      MOZ_CRASH("Unknown type!");
  }

  return IPC_OK();
}

PSmsRequestParent* SmsParent::AllocPSmsRequestParent(
    const IPCSmsRequest& aRequest) {
  SmsRequestParent* actor = new SmsRequestParent();
  // Add an extra ref for IPDL. Will be released in
  // SmsParent::DeallocPSmsRequestParent().
  actor->AddRef();

  return actor;
}

bool SmsParent::DeallocPSmsRequestParent(PSmsRequestParent* aActor) {
  // SmsRequestParent is refcounted, must not be freed manually.
  static_cast<SmsRequestParent*>(aActor)->Release();
  return true;
}

IPCResult SmsParent::RecvPMobileMessageCursorConstructor(
    PMobileMessageCursorParent* aActor,
    const IPCMobileMessageCursor& aRequest) {
  MobileMessageCursorParent* actor =
      static_cast<MobileMessageCursorParent*>(aActor);

  switch (aRequest.type()) {
    case IPCMobileMessageCursor::TCreateMessageCursorRequest:
      return actor->DoRequest(aRequest.get_CreateMessageCursorRequest())
                 ? IPC_OK()
                 : IPC_FAIL_NO_REASON(this);
    case IPCMobileMessageCursor::TCreateThreadCursorRequest:
      return actor->DoRequest(aRequest.get_CreateThreadCursorRequest())
                 ? IPC_OK()
                 : IPC_FAIL_NO_REASON(this);
    default:
      MOZ_CRASH("Unknown type!");
  }

  return IPC_OK();
}

PMobileMessageCursorParent* SmsParent::AllocPMobileMessageCursorParent(
    const IPCMobileMessageCursor& aRequest) {
  MobileMessageCursorParent* actor = new MobileMessageCursorParent();
  // Add an extra ref for IPDL. Will be released in
  // SmsParent::DeallocPMobileMessageCursorParent().
  actor->AddRef();

  return actor;
}

bool SmsParent::DeallocPMobileMessageCursorParent(
    PMobileMessageCursorParent* aActor) {
  // MobileMessageCursorParent is refcounted, must not be freed manually.
  static_cast<MobileMessageCursorParent*>(aActor)->Release();
  return true;
}

/*******************************************************************************
 * SmsRequestParent
 ******************************************************************************/

NS_IMPL_ISUPPORTS(SmsRequestParent, nsIMobileMessageCallback)

void SmsRequestParent::ActorDestroy(ActorDestroyReason aWhy) {
  mActorDestroyed = true;
}

bool SmsRequestParent::DoRequest(const SendMessageRequest& aRequest) {
  switch (aRequest.type()) {
    case SendMessageRequest::TSendSmsMessageRequest: {
      nsCOMPtr<nsISmsService> smsService =
          do_GetService(SMS_SERVICE_CONTRACTID);
      NS_ENSURE_TRUE(smsService, true);

      const SendSmsMessageRequest& req = aRequest.get_SendSmsMessageRequest();
      smsService->Send(req.serviceId(), req.number(), req.message(),
                       req.silent(), this);
    } break;
    case SendMessageRequest::TSendMmsMessageRequest: {
      nsCOMPtr<nsIMmsService> mmsService =
          do_GetService(MMS_SERVICE_CONTRACTID);
      NS_ENSURE_TRUE(mmsService, true);

      // There are cases (see bug 981202) where this is called with no JS on the
      // stack. And since mmsService might be JS-Implemented, we need to pass a
      // jsval to ::Send. Only system code should be looking at the result here,
      // so we just create it in the System-Principaled Junk Scope.
      AutoJSContext cx;
      // JSAutoCompartment ac(cx, xpc::PrivilegedJunkScope());
      JSAutoRealm ar(cx, xpc::PrivilegedJunkScope());
      JS::Rooted<JS::Value> params(cx);
      const SendMmsMessageRequest& req = aRequest.get_SendMmsMessageRequest();
      if (!GetParamsFromSendMmsMessageRequest(cx, req, params.address())) {
        NS_WARNING("SmsRequestParent: Fail to build MMS params.");
        return true;
      }

      mmsService->Send(req.serviceId(), params, this);
    } break;
    default:
      MOZ_CRASH("Unknown type of SendMessageRequest!");
  }
  return true;
}

bool SmsRequestParent::DoRequest(const RetrieveMessageRequest& aRequest) {
  nsresult rv = NS_ERROR_FAILURE;

  nsCOMPtr<nsIMmsService> mmsService = do_GetService(MMS_SERVICE_CONTRACTID);
  if (mmsService) {
    rv = mmsService->Retrieve(aRequest.messageId(), this);
  }

  if (NS_FAILED(rv)) {
    return NS_SUCCEEDED(
        NotifyGetMessageFailed(nsIMobileMessageCallback::INTERNAL_ERROR));
  }

  return true;
}

bool SmsRequestParent::DoRequest(const GetMessageRequest& aRequest) {
  nsresult rv = NS_ERROR_FAILURE;

  nsCOMPtr<nsIMobileMessageDatabaseService> dbService =
      do_GetService(MOBILE_MESSAGE_DATABASE_SERVICE_CONTRACTID);
  if (dbService) {
    rv = dbService->GetMessageMoz(aRequest.messageId(), this);
  }

  if (NS_FAILED(rv)) {
    return NS_SUCCEEDED(
        NotifyGetMessageFailed(nsIMobileMessageCallback::INTERNAL_ERROR));
  }

  return true;
}

bool SmsRequestParent::DoRequest(const GetSmscAddressRequest& aRequest) {
  nsresult rv = NS_ERROR_FAILURE;

  nsCOMPtr<nsISmsService> smsService = do_GetService(SMS_SERVICE_CONTRACTID);
  if (smsService) {
    rv = smsService->GetSmscAddress(aRequest.serviceId(), this);
  }

  if (NS_FAILED(rv)) {
    return NS_SUCCEEDED(
        NotifyGetSmscAddressFailed(nsIMobileMessageCallback::INTERNAL_ERROR));
  }

  return true;
}

bool SmsRequestParent::DoRequest(const SetSmscAddressRequest& aRequest) {
  nsresult rv = NS_ERROR_FAILURE;

  nsCOMPtr<nsISmsService> smsService = do_GetService(SMS_SERVICE_CONTRACTID);
  if (smsService) {
    rv = smsService->SetSmscAddress(aRequest.serviceId(), aRequest.number(),
                                    aRequest.typeOfNumber(),
                                    aRequest.numberPlanIdentification(), this);
  } else {
    return NS_SUCCEEDED(
        NotifySetSmscAddressFailed(nsIMobileMessageCallback::INTERNAL_ERROR));
  }

  if (NS_FAILED(rv)) {
    return NS_SUCCEEDED(
        NotifySetSmscAddressFailed(nsIMobileMessageCallback::INTERNAL_ERROR));
  }

  return true;
}

bool SmsRequestParent::DoRequest(const DeleteMessageRequest& aRequest) {
  nsresult rv = NS_ERROR_FAILURE;

  nsCOMPtr<nsIMobileMessageDatabaseService> dbService =
      do_GetService(MOBILE_MESSAGE_DATABASE_SERVICE_CONTRACTID);
  if (dbService) {
    const nsTArray<int32_t>& messageIds = aRequest.messageIds();
    rv = dbService->DeleteMessage(const_cast<int32_t*>(messageIds.Elements()),
                                  messageIds.Length(), this);
  }

  if (NS_FAILED(rv)) {
    return NS_SUCCEEDED(
        NotifyDeleteMessageFailed(nsIMobileMessageCallback::INTERNAL_ERROR));
  }

  return true;
}

bool SmsRequestParent::DoRequest(const MarkMessageReadRequest& aRequest) {
  nsresult rv = NS_ERROR_FAILURE;

  nsCOMPtr<nsIMobileMessageDatabaseService> dbService =
      do_GetService(MOBILE_MESSAGE_DATABASE_SERVICE_CONTRACTID);
  if (dbService) {
    rv = dbService->MarkMessageRead(aRequest.messageId(), aRequest.value(),
                                    aRequest.sendReadReport(), this);
  }

  if (NS_FAILED(rv)) {
    return NS_SUCCEEDED(
        NotifyMarkMessageReadFailed(nsIMobileMessageCallback::INTERNAL_ERROR));
  }

  return true;
}

bool SmsRequestParent::DoRequest(const GetSegmentInfoForTextRequest& aRequest) {
  nsresult rv = NS_ERROR_FAILURE;

  nsCOMPtr<nsISmsService> smsService = do_GetService(SMS_SERVICE_CONTRACTID);
  if (smsService) {
    rv = smsService->GetSegmentInfoForText(aRequest.text(), this);
  }

  if (NS_FAILED(rv)) {
    return NS_SUCCEEDED(NotifyGetSegmentInfoForTextFailed(
        nsIMobileMessageCallback::INTERNAL_ERROR));
  }

  return true;
}

nsresult SmsRequestParent::SendReply(const MessageReply& aReply) {
  // The child process could die before this asynchronous notification, in which
  // case ActorDestroy() was called and mActorDestroyed is set to true. Return
  // an error here to avoid sending a message to the dead process.
  NS_ENSURE_TRUE(!mActorDestroyed, NS_ERROR_FAILURE);

  return Send__delete__(this, aReply) ? NS_OK : NS_ERROR_FAILURE;
}

// nsIMobileMessageCallback

NS_IMETHODIMP
SmsRequestParent::NotifyMessageSent(nsISupports* aMessage) {
  NS_ENSURE_TRUE(!mActorDestroyed, NS_ERROR_FAILURE);

  MobileMessageData data;
  if (GetMobileMessageDataFromMessage(aMessage, data)) {
    return SendReply(ReplyMessageSend(data));
  }

  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
SmsRequestParent::NotifySendMessageFailed(int32_t aError,
                                          nsISupports* aMessage) {
  NS_ENSURE_TRUE(!mActorDestroyed, NS_ERROR_FAILURE);

  MobileMessageData data;
  if (!GetMobileMessageDataFromMessage(aMessage, data)) {
    return SendReply(
        ReplyMessageSendFail(aError, OptionalMobileMessageData(void_t())));
  }

  return SendReply(
      ReplyMessageSendFail(aError, OptionalMobileMessageData(data)));
}

NS_IMETHODIMP
SmsRequestParent::NotifyMessageGot(nsISupports* aMessage) {
  NS_ENSURE_TRUE(!mActorDestroyed, NS_ERROR_FAILURE);

  MobileMessageData data;
  if (GetMobileMessageDataFromMessage(aMessage, data)) {
    return SendReply(ReplyGetMessage(data));
  }

  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
SmsRequestParent::NotifyGetMessageFailed(int32_t aError) {
  return SendReply(ReplyGetMessageFail(aError));
}

NS_IMETHODIMP
SmsRequestParent::NotifyMessageDeleted(bool* aDeleted, uint32_t aSize) {
  ReplyMessageDelete data;
  data.deleted().AppendElements(aDeleted, aSize);
  return SendReply(data);
}

NS_IMETHODIMP
SmsRequestParent::NotifyDeleteMessageFailed(int32_t aError) {
  return SendReply(ReplyMessageDeleteFail(aError));
}

NS_IMETHODIMP
SmsRequestParent::NotifyMessageMarkedRead(bool aRead) {
  return SendReply(ReplyMarkeMessageRead(aRead));
}

NS_IMETHODIMP
SmsRequestParent::NotifyMarkMessageReadFailed(int32_t aError) {
  return SendReply(ReplyMarkeMessageReadFail(aError));
}

NS_IMETHODIMP
SmsRequestParent::NotifySegmentInfoForTextGot(
    int32_t aSegments, int32_t aCharsPerSegment,
    int32_t aCharsAvailableInLastSegment) {
  return SendReply(ReplyGetSegmentInfoForText(aSegments, aCharsPerSegment,
                                              aCharsAvailableInLastSegment));
}

NS_IMETHODIMP
SmsRequestParent::NotifyGetSegmentInfoForTextFailed(int32_t aError) {
  return SendReply(ReplyGetSegmentInfoForTextFail(aError));
}

NS_IMETHODIMP
SmsRequestParent::NotifyGetSmscAddress(const nsAString& aSmscAddress,
                                       uint32_t aTypeOfNumber,
                                       uint32_t aNumberPlanIdentification) {
  return SendReply(ReplyGetSmscAddress(nsString(aSmscAddress), aTypeOfNumber,
                                       aNumberPlanIdentification));
}

NS_IMETHODIMP
SmsRequestParent::NotifyGetSmscAddressFailed(int32_t aError) {
  return SendReply(ReplyGetSmscAddressFail(aError));
}

NS_IMETHODIMP
SmsRequestParent::NotifySetSmscAddress() {
  return SendReply(ReplySetSmscAddress());
}

NS_IMETHODIMP
SmsRequestParent::NotifySetSmscAddressFailed(int32_t aError) {
  return SendReply(ReplySetSmscAddressFail(aError));
}

/*******************************************************************************
 * MobileMessageCursorParent
 ******************************************************************************/

NS_IMPL_ISUPPORTS(MobileMessageCursorParent, nsIMobileMessageCursorCallback)

void MobileMessageCursorParent::ActorDestroy(ActorDestroyReason aWhy) {
  // Two possible scenarios here:
  // 1) When parent fails to SendNotifyResult() in NotifyCursorResult(), it's
  //    destroyed without nulling out mContinueCallback.
  // 2) When parent dies normally, mContinueCallback should have been cleared in
  //    NotifyCursorError(), but just ensure this again.
  mContinueCallback = nullptr;
}

IPCResult MobileMessageCursorParent::RecvContinue() {
  MOZ_ASSERT(mContinueCallback);

  if (NS_FAILED(mContinueCallback->HandleContinue())) {
    bool success = NS_SUCCEEDED(
        NotifyCursorError(nsIMobileMessageCallback::INTERNAL_ERROR));
    return success ? IPC_OK() : IPC_FAIL(this, "Internal Error");
  }

  return IPC_OK();
}

bool MobileMessageCursorParent::DoRequest(
    const CreateMessageCursorRequest& aRequest) {
  nsresult rv = NS_ERROR_FAILURE;

  nsCOMPtr<nsIMobileMessageDatabaseService> dbService =
      do_GetService(MOBILE_MESSAGE_DATABASE_SERVICE_CONTRACTID);
  if (dbService) {
    const SmsFilterData& filter = aRequest.filter();

    const nsTArray<nsString>& numbers = filter.numbers();
    UniquePtr<const char16_t*[]> ptrNumbers;
    uint32_t numbersCount = numbers.Length();
    if (numbersCount) {
      uint32_t index;

      ptrNumbers = MakeUnique<const char16_t*[]>(numbersCount);
      for (index = 0; index < numbersCount; index++) {
        ptrNumbers[index] = numbers[index].get();
      }
    }

    rv = dbService->CreateMessageCursor(
        filter.hasStartDate(), filter.startDate(), filter.hasEndDate(),
        filter.endDate(), ptrNumbers.get(), numbersCount, filter.delivery(),
        filter.hasRead(), filter.read(), filter.hasThreadId(),
        filter.threadId(), aRequest.reverse(), this,
        getter_AddRefs(mContinueCallback));
  }

  if (NS_FAILED(rv)) {
    return NS_SUCCEEDED(
        NotifyCursorError(nsIMobileMessageCallback::INTERNAL_ERROR));
  }

  return true;
}

bool MobileMessageCursorParent::DoRequest(
    const CreateThreadCursorRequest& aRequest) {
  nsresult rv = NS_ERROR_FAILURE;

  nsCOMPtr<nsIMobileMessageDatabaseService> dbService =
      do_GetService(MOBILE_MESSAGE_DATABASE_SERVICE_CONTRACTID);
  if (dbService) {
    rv = dbService->CreateThreadCursor(this, getter_AddRefs(mContinueCallback));
  }

  if (NS_FAILED(rv)) {
    return NS_SUCCEEDED(
        NotifyCursorError(nsIMobileMessageCallback::INTERNAL_ERROR));
  }

  return true;
}

// nsIMobileMessageCursorCallback

NS_IMETHODIMP
MobileMessageCursorParent::NotifyCursorError(int32_t aError) {
  // The child process could die before this asynchronous notification, in which
  // case ActorDestroy() was called and mContinueCallback is now null. Return an
  // error here to avoid sending a message to the dead process.
  NS_ENSURE_TRUE(mContinueCallback, NS_ERROR_FAILURE);

  mContinueCallback = nullptr;

  return Send__delete__(this, aError) ? NS_OK : NS_ERROR_FAILURE;
}

NS_IMETHODIMP
MobileMessageCursorParent::NotifyCursorResult(nsISupports** aResults,
                                              uint32_t aSize) {
  MOZ_ASSERT(aResults && *aResults && aSize);

  // The child process could die before this asynchronous notification, in which
  // case ActorDestroy() was called and mContinueCallback is now null. Return an
  // error here to avoid sending a message to the dead process.
  NS_ENSURE_TRUE(mContinueCallback, NS_ERROR_FAILURE);

  nsCOMPtr<nsIMobileMessageThread> iThread = do_QueryInterface(aResults[0]);
  if (iThread) {
    nsTArray<ThreadData> threads;

    for (uint32_t i = 0; i < aSize; i++) {
      nsCOMPtr<nsIMobileMessageThread> iThread = do_QueryInterface(aResults[i]);
      NS_ENSURE_TRUE(iThread, NS_ERROR_FAILURE);

      MobileMessageThreadInternal* thread =
          static_cast<MobileMessageThreadInternal*>(iThread.get());
      threads.AppendElement(thread->GetData());
    }

    return SendNotifyResult(MobileMessageCursorData(ThreadArrayData(threads)))
               ? NS_OK
               : NS_ERROR_FAILURE;
  }

  nsTArray<MobileMessageData> messages;
  for (uint32_t i = 0; i < aSize; i++) {
    nsCOMPtr<nsISmsMessage> sms = do_QueryInterface(aResults[i]);
    if (sms) {
      messages.AppendElement(
          static_cast<SmsMessageInternal*>(sms.get())->GetData());
      continue;
    }

    nsCOMPtr<nsIMmsMessage> mms = do_QueryInterface(aResults[i]);
    if (mms) {
      MmsMessageData mmsData;
      NS_ENSURE_TRUE(
          static_cast<MmsMessageInternal*>(mms.get())->GetData(mmsData),
          NS_ERROR_FAILURE);
      messages.AppendElement(mmsData);
      continue;
    }

    return NS_ERROR_FAILURE;
  }

  return SendNotifyResult(
             MobileMessageCursorData(MobileMessageArrayData(messages)))
             ? NS_OK
             : NS_ERROR_FAILURE;
}

NS_IMETHODIMP
MobileMessageCursorParent::NotifyCursorDone() {
  return NotifyCursorError(nsIMobileMessageCallback::SUCCESS_NO_ERROR);
}

}  // namespace mobilemessage
}  // namespace dom
}  // namespace mozilla
