/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "RemoteVideoDecoder.h"

#include "mozilla/layers/ImageDataSerializer.h"

#ifdef MOZ_AV1
#  include "AOMDecoder.h"
#  include "DAV1DDecoder.h"
#endif
#ifdef XP_WIN
#  include "WMFDecoderModule.h"
#endif
#include "ImageContainer.h"  // for PlanarYCbCrData and BufferRecycleBin
#include "mozilla/layers/VideoBridgeChild.h"
#include "mozilla/layers/ImageClient.h"
#include "PDMFactory.h"
#include "RemoteDecoderManagerChild.h"
#include "RemoteDecoderManagerParent.h"
#include "GPUVideoImage.h"
#include "MediaInfo.h"
#include "mozilla/Telemetry.h"
#include "mozilla/layers/TextureClient.h"

namespace mozilla {

using namespace layers;  // for PlanarYCbCrData and BufferRecycleBin
using namespace ipc;
using namespace gfx;

class KnowsCompositorVideo : public layers::KnowsCompositor {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(KnowsCompositorVideo, override)

  layers::TextureForwarder* GetTextureForwarder() override {
    return VideoBridgeChild::GetSingleton();
  }
  layers::LayersIPCActor* GetLayersIPCActor() override {
    return VideoBridgeChild::GetSingleton();
  }

 private:
  virtual ~KnowsCompositorVideo() = default;
};

RemoteVideoDecoderChild::RemoteVideoDecoderChild(bool aRecreatedOnCrash)
    : RemoteDecoderChild(aRecreatedOnCrash),
      mBufferRecycleBin(new BufferRecycleBin) {}

RefPtr<mozilla::layers::Image> RemoteVideoDecoderChild::DeserializeImage(
    const SurfaceDescriptorBuffer& aSdBuffer, const IntSize& aPicSize) {
  MOZ_ASSERT(aSdBuffer.desc().type() == BufferDescriptor::TYCbCrDescriptor);
  if (aSdBuffer.desc().type() != BufferDescriptor::TYCbCrDescriptor) {
    return nullptr;
  }
  const YCbCrDescriptor& descriptor = aSdBuffer.desc().get_YCbCrDescriptor();

  uint8_t* buffer = nullptr;
  const MemoryOrShmem& memOrShmem = aSdBuffer.data();
  switch (memOrShmem.type()) {
    case MemoryOrShmem::Tuintptr_t:
      buffer = reinterpret_cast<uint8_t*>(memOrShmem.get_uintptr_t());
      break;
    case MemoryOrShmem::TShmem:
      buffer = memOrShmem.get_Shmem().get<uint8_t>();
      break;
    default:
      MOZ_ASSERT(false, "Unknown MemoryOrShmem type");
  }
  if (!buffer) {
    return nullptr;
  }

  PlanarYCbCrData pData;
  pData.mYSize = descriptor.ySize();
  pData.mYStride = descriptor.yStride();
  pData.mCbCrSize = descriptor.cbCrSize();
  pData.mCbCrStride = descriptor.cbCrStride();
  // default mYSkip, mCbSkip, mCrSkip because not held in YCbCrDescriptor
  pData.mYSkip = pData.mCbSkip = pData.mCrSkip = 0;
  // default mPicX, mPicY because not held in YCbCrDescriptor
  pData.mPicX = pData.mPicY = 0;
  pData.mPicSize = aPicSize;
  pData.mStereoMode = descriptor.stereoMode();
  pData.mColorDepth = descriptor.colorDepth();
  pData.mYUVColorSpace = descriptor.yUVColorSpace();
  pData.mYChannel = ImageDataSerializer::GetYChannel(buffer, descriptor);
  pData.mCbChannel = ImageDataSerializer::GetCbChannel(buffer, descriptor);
  pData.mCrChannel = ImageDataSerializer::GetCrChannel(buffer, descriptor);

  // images coming from AOMDecoder are RecyclingPlanarYCbCrImages.
  RefPtr<RecyclingPlanarYCbCrImage> image =
      new RecyclingPlanarYCbCrImage(mBufferRecycleBin);
  bool setData = image->CopyData(pData);
  MOZ_ASSERT(setData);

  switch (memOrShmem.type()) {
    case MemoryOrShmem::Tuintptr_t:
      delete[] reinterpret_cast<uint8_t*>(memOrShmem.get_uintptr_t());
      break;
    case MemoryOrShmem::TShmem:
      DeallocShmem(memOrShmem.get_Shmem());
      break;
    default:
      MOZ_ASSERT(false, "Unknown MemoryOrShmem type");
  }

  if (!setData) {
    return nullptr;
  }

  return image;
}

mozilla::ipc::IPCResult RemoteVideoDecoderChild::RecvOutput(
    const DecodedOutputIPDL& aDecodedData) {
  AssertOnManagerThread();
  MOZ_ASSERT(aDecodedData.type() == DecodedOutputIPDL::TRemoteVideoDataIPDL);
  const RemoteVideoDataIPDL& aData = aDecodedData.get_RemoteVideoDataIPDL();

  RefPtr<Image> image = DeserializeImage(
      aData.sd().get_SurfaceDescriptorBuffer(), aData.frameSize());

  RefPtr<VideoData> video = VideoData::CreateFromImage(
      aData.display(), aData.base().offset(), aData.base().time(),
      aData.base().duration(), image, aData.base().keyframe(),
      aData.base().timecode());

  mDecodedData.AppendElement(std::move(video));
  return IPC_OK();
}

MediaResult RemoteVideoDecoderChild::InitIPDL(
    const VideoInfo& aVideoInfo, float aFramerate,
    const CreateDecoderParams::OptionSet& aOptions) {
  RefPtr<RemoteDecoderManagerChild> manager =
      RemoteDecoderManagerChild::GetRDDProcessSingleton();

  // The manager isn't available because RemoteDecoderManagerChild has been
  // initialized with null end points and we don't want to decode video on RDD
  // process anymore. Return false here so that we can fallback to other PDMs.
  if (!manager) {
    return MediaResult(NS_ERROR_DOM_MEDIA_FATAL_ERR,
                       RESULT_DETAIL("RemoteDecoderManager is not available."));
  }

  if (!manager->CanSend()) {
    return MediaResult(NS_ERROR_DOM_MEDIA_FATAL_ERR,
                       RESULT_DETAIL("RemoteDecoderManager unable to send."));
  }

  mIPDLSelfRef = this;
  bool success = false;
  nsCString errorDescription;
  nsCString blacklistedD3D11Driver;
  nsCString blacklistedD3D9Driver;
  VideoDecoderInfoIPDL decoderInfo(aVideoInfo, aFramerate);
  TextureFactoryIdentifier defaultIdent;
  if (manager->SendPRemoteDecoderConstructor(
          this, decoderInfo, aOptions, defaultIdent, &success,
          &blacklistedD3D11Driver, &blacklistedD3D9Driver, &errorDescription)) {
    mCanSend = true;
  }

  return success ? MediaResult(NS_OK)
                 : MediaResult(NS_ERROR_DOM_MEDIA_FATAL_ERR, errorDescription);
}

#ifdef XP_WIN
static void ReportUnblacklistingTelemetry(
    bool isGPUProcessCrashed, const nsCString& aD3D11BlacklistedDriver,
    const nsCString& aD3D9BlacklistedDriver) {
  const nsCString& blacklistedDLL = !aD3D11BlacklistedDriver.IsEmpty()
                                        ? aD3D11BlacklistedDriver
                                        : aD3D9BlacklistedDriver;

  if (!blacklistedDLL.IsEmpty()) {
    Telemetry::Accumulate(
        Telemetry::VIDEO_UNBLACKINGLISTING_DXVA_DRIVER_RUNTIME_STATUS,
        blacklistedDLL, isGPUProcessCrashed ? 1 : 0);
  }
}
#endif  // XP_WIN

GpuRemoteVideoDecoderChild::GpuRemoteVideoDecoderChild()
    : RemoteVideoDecoderChild(true) {}

mozilla::ipc::IPCResult GpuRemoteVideoDecoderChild::RecvOutput(
    const DecodedOutputIPDL& aDecodedData) {
  AssertOnManagerThread();
  MOZ_ASSERT(aDecodedData.type() == DecodedOutputIPDL::TRemoteVideoDataIPDL);
  const RemoteVideoDataIPDL& aData = aDecodedData.get_RemoteVideoDataIPDL();

  // The Image here creates a TextureData object that takes ownership
  // of the SurfaceDescriptor, and is responsible for making sure that
  // it gets deallocated.
  RefPtr<Image> image =
      new GPUVideoImage(GetManager(), aData.sd(), aData.frameSize());

  RefPtr<VideoData> video = VideoData::CreateFromImage(
      aData.display(), aData.base().offset(), aData.base().time(),
      aData.base().duration(), image, aData.base().keyframe(),
      aData.base().timecode());

  mDecodedData.AppendElement(std::move(video));
  return IPC_OK();
}

void GpuRemoteVideoDecoderChild::RecordShutdownTelemetry(
    bool aAbnormalShutdown) {
#ifdef XP_WIN
  ReportUnblacklistingTelemetry(aAbnormalShutdown, mBlacklistedD3D11Driver,
                                mBlacklistedD3D9Driver);
#endif  // XP_WIN
}

MediaResult GpuRemoteVideoDecoderChild::InitIPDL(
    const VideoInfo& aVideoInfo, float aFramerate,
    const CreateDecoderParams::OptionSet& aOptions,
    const layers::TextureFactoryIdentifier& aIdentifier) {
  RefPtr<RemoteDecoderManagerChild> manager =
      RemoteDecoderManagerChild::GetGPUProcessSingleton();

  // The manager isn't available because RemoteDecoderManagerChild has been
  // initialized with null end points and we don't want to decode video on GPU
  // process anymore. Return false here so that we can fallback to other PDMs.
  if (!manager) {
    return MediaResult(NS_ERROR_DOM_MEDIA_FATAL_ERR,
                       RESULT_DETAIL("RemoteDecoderManager is not available."));
  }

  // The manager doesn't support sending messages because we've just crashed
  // and are working on reinitialization. Don't initialize mIPDLSelfRef and
  // leave us in an error state. We'll then immediately reject the promise when
  // Init() is called and the caller can try again. Hopefully by then the new
  // manager is ready, or we've notified the caller of it being no longer
  // available. If not, then the cycle repeats until we're ready.
  if (!manager->CanSend()) {
    return NS_OK;
  }

  mIPDLSelfRef = this;
  bool success = false;
  nsCString errorDescription;
  VideoDecoderInfoIPDL decoderInfo(aVideoInfo, aFramerate);
  if (manager->SendPRemoteDecoderConstructor(
          this, decoderInfo, aOptions, aIdentifier, &success,
          &mBlacklistedD3D11Driver, &mBlacklistedD3D9Driver,
          &errorDescription)) {
    mCanSend = true;
  }

  return success ? MediaResult(NS_OK)
                 : MediaResult(NS_ERROR_DOM_MEDIA_FATAL_ERR, errorDescription);
}

RemoteVideoDecoderParent::RemoteVideoDecoderParent(
    RemoteDecoderManagerParent* aParent, const VideoInfo& aVideoInfo,
    float aFramerate, const CreateDecoderParams::OptionSet& aOptions,
    const layers::TextureFactoryIdentifier& aIdentifier,
    TaskQueue* aManagerTaskQueue, TaskQueue* aDecodeTaskQueue, bool* aSuccess,
    nsCString* aErrorDescription)
    : RemoteDecoderParent(aParent, aManagerTaskQueue, aDecodeTaskQueue),
      mVideoInfo(aVideoInfo) {
  if (XRE_IsGPUProcess()) {
    mKnowsCompositor = new KnowsCompositorVideo();
    mKnowsCompositor->IdentifyTextureHost(aIdentifier);
  }

  CreateDecoderParams params(mVideoInfo);
  params.mTaskQueue = mDecodeTaskQueue;
  params.mKnowsCompositor = mKnowsCompositor;
  params.mImageContainer = new layers::ImageContainer();
  params.mRate = CreateDecoderParams::VideoFrameRate(aFramerate);
  params.mOptions = aOptions;
  MediaResult error(NS_OK);
  params.mError = &error;

  if (XRE_IsGPUProcess()) {
#ifdef XP_WIN
    // Ensure everything is properly initialized on the right thread.
    PDMFactory::EnsureInit();

    // TODO: Ideally we wouldn't hardcode the WMF PDM, and we'd use the normal
    // PDM factory logic for picking a decoder.
    RefPtr<WMFDecoderModule> pdm(new WMFDecoderModule());
    pdm->Startup();
    mDecoder = pdm->CreateVideoDecoder(params);
#else
    MOZ_ASSERT(false,
               "Can't use RemoteVideoDecoder in the GPU process on non-Windows "
               "platforms yet");
#endif
  }

#ifdef MOZ_AV1
  if (AOMDecoder::IsAV1(params.mConfig.mMimeType)) {
    if (StaticPrefs::MediaAv1UseDav1d()) {
      mDecoder = new DAV1DDecoder(params);
    } else {
      mDecoder = new AOMDecoder(params);
    }
  }
#endif

  if (NS_FAILED(error)) {
    MOZ_ASSERT(aErrorDescription);
    *aErrorDescription = error.Description();
  }

  *aSuccess = !!mDecoder;
}

MediaResult RemoteVideoDecoderParent::ProcessDecodedData(
    const MediaDataDecoder::DecodedData& aData) {
  MOZ_ASSERT(OnManagerThread());

  // If the video decoder bridge has shut down, stop.
  if (mKnowsCompositor && !mKnowsCompositor->GetTextureForwarder()) {
    return NS_OK;
  }

  for (const auto& data : aData) {
    MOZ_ASSERT(data->mType == MediaData::Type::VIDEO_DATA,
               "Can only decode videos using RemoteDecoderParent!");
    VideoData* video = static_cast<VideoData*>(data.get());

    MOZ_ASSERT(video->mImage,
               "Decoded video must output a layer::Image to "
               "be used with RemoteDecoderParent");

    SurfaceDescriptor sd;
    IntSize size;

    if (mKnowsCompositor) {
      RefPtr<TextureClient> texture =
          video->mImage->GetTextureClient(mKnowsCompositor);

      if (!texture) {
        texture = ImageClient::CreateTextureClientForImage(video->mImage,
                                                           mKnowsCompositor);
      }

      if (texture && !texture->IsAddedToCompositableClient()) {
        texture->InitIPDLActor(mKnowsCompositor);
        texture->SetAddedToCompositableClient();
      }
      if (texture) {
        sd = mParent->StoreImage(video->mImage, texture);
        size = texture->GetSize();
      }
    } else {
      PlanarYCbCrImage* image =
          static_cast<PlanarYCbCrImage*>(video->mImage.get());

      SurfaceDescriptorBuffer sdBuffer;
      Shmem buffer;
      if (!AllocShmem(image->GetDataSize(), Shmem::SharedMemory::TYPE_BASIC,
                      &buffer)) {
        return MediaResult(NS_ERROR_OUT_OF_MEMORY,
                           "AllocShmem failed in "
                           "RemoteVideoDecoderParent::ProcessDecodedData");
      }
      if (image->GetDataSize() > buffer.Size<uint8_t>()) {
        return MediaResult(NS_ERROR_OUT_OF_MEMORY,
                           "AllocShmem returned less than requested in "
                           "RemoteVideoDecoderParent::ProcessDecodedData");
      }

      sdBuffer.data() = std::move(buffer);
      image->BuildSurfaceDescriptorBuffer(sdBuffer);

      sd = sdBuffer;
      size = image->GetSize();
    }

    RemoteVideoDataIPDL output(
        MediaDataIPDL(data->mOffset, data->mTime, data->mTimecode,
                      data->mDuration, data->mKeyframe),
        video->mDisplay, size, sd, video->mFrameID);
    Unused << SendOutput(output);
  }

  return NS_OK;
}

}  // namespace mozilla
