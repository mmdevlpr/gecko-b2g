/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "GonkDecoderModule.h"

#include "GonkVideoDecoderManager.h"
#include "GonkAudioDecoderManager.h"
#include "GonkMediaDataDecoder.h"
#include "mozilla/CDMProxy.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Preferences.h"

#ifdef B2G_MEDIADRM
#  include "EMEDecoderModule.h"
#endif

namespace mozilla {
GonkDecoderModule::GonkDecoderModule(CDMProxy* aProxy) : mCDMProxy(aProxy) {}

/* static */
already_AddRefed<PlatformDecoderModule> GonkDecoderModule::Create(
    CDMProxy* aProxy) {
  return MakeAndAddRef<GonkDecoderModule>(aProxy);
}

already_AddRefed<MediaDataDecoder> GonkDecoderModule::CreateVideoDecoder(
    const CreateDecoderParams& aParams) {
  RefPtr<MediaDataDecoder> decoder =
      new GonkMediaDataDecoder(new GonkVideoDecoderManager(
          aParams.VideoConfig(), aParams.mImageContainer, mCDMProxy.get()));
#ifdef B2G_MEDIADRM
  if (mCDMProxy) {
    decoder = new EMEMediaDataDecoderProxy(aParams, decoder.forget(),
                                           mCDMProxy.get());
  }
#endif
  return decoder.forget();
}

already_AddRefed<MediaDataDecoder> GonkDecoderModule::CreateAudioDecoder(
    const CreateDecoderParams& aParams) {
  RefPtr<MediaDataDecoder> decoder = new GonkMediaDataDecoder(
      new GonkAudioDecoderManager(aParams.AudioConfig(), mCDMProxy.get()));
#ifdef B2G_MEDIADRM
  if (mCDMProxy) {
    decoder = new EMEMediaDataDecoderProxy(aParams, decoder.forget(),
                                           mCDMProxy.get());
  }
#endif
  return decoder.forget();
}

media::DecodeSupportSet GonkDecoderModule::SupportsMimeType(
    const nsACString& aMimeType, DecoderDoctorDiagnostics* aDiagnostics) const {
    if (aMimeType.EqualsLiteral("audio/mp4a-latm") ||
        aMimeType.EqualsLiteral("audio/aac") ||
        aMimeType.EqualsLiteral("audio/mp4") ||
        aMimeType.EqualsLiteral("audio/3gpp") ||
        aMimeType.EqualsLiteral("audio/amr-wb") ||
        aMimeType.EqualsLiteral("audio/mpeg") ||
        aMimeType.EqualsLiteral("audio/flac") ||
        aMimeType.EqualsLiteral("video/mp4") ||
        aMimeType.EqualsLiteral("video/mp4v-es") ||
        aMimeType.EqualsLiteral("video/avc") ||
        aMimeType.EqualsLiteral("video/3gpp")) {
        return media::DecodeSupport::SoftwareDecode;
    }
    return media::DecodeSupport::SoftwareDecode;
}

}  // namespace mozilla
