/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim:set ts=4 sw=4 sts=4 et: */
/*
 * Copyright (c) 2012, 2013 The Linux Foundation. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <android/log.h>
#include <string.h>

#include "libdisplay/GonkDisplay.h"
#include "HwcComposer2D.h"
// #include "LayerScope.h"
#include "Units.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/layers/CompositorBridgeParent.h"
// #include "mozilla/layers/LayerManagerComposite.h"
// #include "mozilla/layers/PLayerTransaction.h"
//#include "mozilla/layers/ShadowLayerUtilsGralloc.h"
#include "mozilla/layers/TextureHostOGL.h"  // for TextureHostOGL
#include "mozilla/StaticPtr.h"
#include "nsIScreen.h"
#include "nsThreadUtils.h"
#include "cutils/properties.h"
#include "gfx2DGlue.h"
#include "gfxPlatform.h"
#include "VsyncSource.h"
#include "ScreenHelperGonk.h"
#include "nsWindow.h"

#include "libdisplay/DisplaySurface.h"

#ifdef LOG_TAG
#  undef LOG_TAG
#endif
#define LOG_TAG "HWComposer"

/*
 * By default the debug message of hwcomposer (LOG_DEBUG level) are undefined,
 * but can be enabled by uncommenting HWC_DEBUG below.
 */
//#define HWC_DEBUG

#ifdef HWC_DEBUG
#  define LOGD(args...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, ##args)
#else
#  define LOGD(args...) ((void)0)
#endif

#define LOGI(args...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, ##args)
#define LOGE(args...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, ##args)

#define LAYER_COUNT_INCREMENTS 5

using namespace android;
using namespace mozilla::gfx;
using namespace mozilla::layers;

namespace mozilla {

static void HookInvalidate() { HwcComposer2D::GetInstance()->Invalidate(); }

static void HookVsync(int aDisplay, int64_t aTimestamp) {
  HwcComposer2D::GetInstance()->Vsync(aDisplay, aTimestamp);
}

static void HookHotplug(int aDisplay, int aConnected) {
  HwcComposer2D::GetInstance()->Hotplug(aDisplay, aConnected);
}

__attribute__((visibility("default"))) void HookSetVsyncAlwaysEnabled(
    bool aAlways) {
  HwcComposer2D::GetInstance()->SetVsyncAlwaysEnabled(aAlways);
}

static StaticRefPtr<HwcComposer2D> sInstance;

HwcComposer2D::HwcComposer2D()
    : mList(nullptr),
      mMaxLayerCount(0),
      mColorFill(false),
      mRBSwapSupport(false),
      mPrepared(false),
      mHasHWVsync(false),
      mStopRenderWithHwc(false),
      mAlwaysEnabled(false),
      mLock("mozilla.HwcComposer2D.mLock") {
  mHal = HwcHALBase::CreateHwcHAL();
  if (!mHal->HasHwc()) {
    LOGD("no hwc support");
    return;
  }

  RegisterHwcEventCallback();

  nsIntSize screenSize;

  GonkDisplay::NativeData data =
      GetGonkDisplay()->GetNativeData(DisplayType::DISPLAY_PRIMARY);
  ANativeWindow* win = data.mNativeWindow.get();
  win->query(win, NATIVE_WINDOW_WIDTH, &screenSize.width);
  win->query(win, NATIVE_WINDOW_HEIGHT, &screenSize.height);
  mScreenRect = gfx::IntRect(gfx::IntPoint(0, 0), screenSize);

  mColorFill = mHal->Query(HwcHALBase::QueryType::COLOR_FILL);
  mRBSwapSupport = mHal->Query(HwcHALBase::QueryType::RB_SWAP);
}

HwcComposer2D::~HwcComposer2D() { free(mList); }

HwcComposer2D* HwcComposer2D::GetInstance() {
  if (!sInstance) {
#ifdef HWC_DEBUG
    // Make sure only create once
    static int timesCreated = 0;
    ++timesCreated;
    MOZ_ASSERT(timesCreated == 1);
#endif
    sInstance = new HwcComposer2D();

    // If anyone uses the compositor thread to create HwcComposer2D,
    // we just skip this function.
    // If ClearOnShutdown() can handle objects in other threads
    // in the future, we can remove this check.
    if (NS_IsMainThread()) {
      // If we create HwcComposer2D by the main thread, we can use
      // ClearOnShutdown() to make sure it will be nullified properly.
      ClearOnShutdown(&sInstance);
    }
  }
  return sInstance;
}

bool HwcComposer2D::EnableVsync(bool aEnable) {
  MOZ_ASSERT(NS_IsMainThread());
  if (!mHasHWVsync) {
    return false;
  }

  // Early return if try to disable vsync when mAlwaysEnabled is true
  if (mAlwaysEnabled && !aEnable) {
    return true;
  }

  return mHal->EnableVsync(aEnable) && aEnable;
}

bool HwcComposer2D::RegisterHwcEventCallback() {
  const HwcHALProcs_t cHWCProcs = {
      &HookInvalidate,  // 1st: void (*invalidate)(...)
      &HookVsync,       // 2nd: void (*vsync)(...)
      &HookHotplug      // 3rd: void (*hotplug)(...)
  };
  mHasHWVsync = mHal->RegisterHwcEventCallback(cHWCProcs);
  return mHasHWVsync;
}

void HwcComposer2D::Vsync(int aDisplay, nsecs_t aVsyncTimestamp) {
  // KaiOS Bug 567: The vsync here might be fired during testing whether vsync
  // is avaliabe in gfxAndroidPlatform::GetGonkVsyncSource. At this
  // timing created VsyncSource doesn't be assigned to gfxPlatform yet.

  RefPtr<VsyncSource> vsyncSource = gfxPlatform::GetPlatform()->GetGonkVsyncSource();

  if (!gfxPlatform::Initialized() ||
      !vsyncSource) {
    return;
  }

  TimeStamp vsyncTime = mozilla::TimeStamp::FromSystemTime(aVsyncTimestamp);
  TimeStamp outputTime = vsyncTime + vsyncSource->GetVsyncRate();
  vsyncSource->NotifyVsync(vsyncTime, outputTime);
}

// Called on the "invalidator" thread (run from HAL).
void HwcComposer2D::Invalidate() {
  if (!mHal->HasHwc()) {
    LOGE("HwcComposer2D::Invalidate failed!");
    return;
  }

  MutexAutoLock lock(mLock);
  if (mCompositorBridgeParent) {
    // TODO: should we use a better reason?
    mCompositorBridgeParent->ScheduleRenderOnCompositorThread(
        wr::RenderReasons::OTHER);
  }
}

namespace {
class HotplugEvent : public mozilla::Runnable {
 public:
  HotplugEvent(int displayId, DisplayType aType, bool aConnected)
      : mozilla::Runnable("HotplugEvent"),
        mId(displayId),
        mType(aType),
        mConnected(aConnected) {}

  NS_IMETHOD Run() {
    widget::ScreenHelperGonk* screenHelper =
        widget::ScreenHelperGonk::GetSingleton();
    if (mConnected) {
      screenHelper->AddScreen(mId, mType);
    } else {
      screenHelper->RemoveScreen(mId);
    }
    return NS_OK;
  }

 private:
  int mId;
  DisplayType mType;
  bool mConnected;
};
}  // namespace

void HwcComposer2D::Hotplug(int aDisplay, int aConnected) {
  NS_DispatchToMainThread(
      new HotplugEvent(aDisplay, DisplayType::DISPLAY_EXTERNAL, aConnected));
}

void HwcComposer2D::SetCompositorBridgeParent(
    CompositorBridgeParent* aCompositorBridgeParent) {
  MutexAutoLock lock(mLock);
  mCompositorBridgeParent = aCompositorBridgeParent;
}

bool HwcComposer2D::ReallocLayerList() {
  int size = sizeof(HwcList) +
             ((mMaxLayerCount + LAYER_COUNT_INCREMENTS) * sizeof(HwcLayer));

  HwcList* listrealloc = (HwcList*)realloc(mList, size);

  if (!listrealloc) {
    return false;
  }

  if (!mList) {
    // first alloc, initialize
    listrealloc->numHwLayers = 0;
    listrealloc->flags = 0;
  }

  mList = listrealloc;
  mMaxLayerCount += LAYER_COUNT_INCREMENTS;
  return true;
}

bool HwcComposer2D::PrepareLayerList(Layer* aLayer, const nsIntRect& aClip,
                                     const Matrix& aParentTransform,
                                     bool aFindSidebandStreams) {
  // TODO: FIXME
  return false;
#if 0
    // NB: we fall off this path whenever there are container layers
    // that require intermediate surfaces.  That means all the
    // GetEffective*() coordinates are relative to the framebuffer.

    bool fillColor = false;

    const nsIntRegion visibleRegion = aLayer->GetLocalVisibleRegion().ToUnknownRegion();
    if (visibleRegion.IsEmpty()) {
        return true;
    }

    uint8_t opacity = std::min(0xFF, (int)(aLayer->GetEffectiveOpacity() * 256.0));
    if (opacity == 0) {
        LOGD("%s Layer has zero opacity; skipping", aLayer->Name());
        return true;
    }

    if (!mHal->SupportTransparency() && opacity < 0xFF && !aFindSidebandStreams) {
        LOGD("%s Layer has planar semitransparency which is unsupported by hwcomposer", aLayer->Name());
        return false;
    }

    if (aLayer->GetMaskLayer() && !aFindSidebandStreams) {
        LOGD("%s Layer has MaskLayer which is unsupported by hwcomposer", aLayer->Name());
        return false;
    }

    nsIntRect clip;
    nsIntRect layerClip = aLayer->GetEffectiveClipRect().valueOr(ParentLayerIntRect()).ToUnknownRect();
    nsIntRect* layerClipPtr = aLayer->GetEffectiveClipRect() ? &layerClip : nullptr;
    if (!HwcUtils::CalculateClipRect(aParentTransform,
                                     layerClipPtr,
                                     aClip,
                                     &clip))
    {
        LOGD("%s Clip rect is empty. Skip layer", aLayer->Name());
        return true;
    }

    // HWC supports only the following 2D transformations:
    //
    // Scaling via the sourceCrop and displayFrame in HwcLayer
    // Translation via the sourceCrop and displayFrame in HwcLayer
    // Rotation (in square angles only) via the HWC_TRANSFORM_ROT_* flags
    // Reflection (horizontal and vertical) via the HWC_TRANSFORM_FLIP_* flags
    //
    // A 2D transform with PreservesAxisAlignedRectangles() has all the attributes
    // above
    Matrix layerTransform;
    if (!aLayer->GetEffectiveTransform().Is2D(&layerTransform) ||
        !layerTransform.PreservesAxisAlignedRectangles()) {
        LOGD("Layer EffectiveTransform has a 3D transform or a non-square angle rotation");
        return false;
    }

    Matrix layerBufferTransform;
    if (!aLayer->GetEffectiveTransformForBuffer().Is2D(&layerBufferTransform) ||
        !layerBufferTransform.PreservesAxisAlignedRectangles()) {
        LOGD("Layer EffectiveTransformForBuffer has a 3D transform or a non-square angle rotation");
      return false;
    }

    if (ContainerLayer* container = aLayer->AsContainerLayer()) {
        if (container->UseIntermediateSurface() && !aFindSidebandStreams) {
            LOGD("Container layer needs intermediate surface");
            return false;
        }
        AutoTArray<Layer*, 12> children;
        container->SortChildrenBy3DZOrder(children);

        for (uint32_t i = 0; i < children.Length(); i++) {
            if (!PrepareLayerList(children[i], clip, layerTransform, aFindSidebandStreams) &&
                !aFindSidebandStreams) {
                return false;
            }
        }
        return true;
    }

    LayerRenderState state = aLayer->GetRenderState();

    if (!state.GetGrallocBuffer() && !state.GetSidebandStream().IsValid()) {
        if (aLayer->AsColorLayer() && mColorFill) {
            fillColor = true;
        } else {
            LOGD("%s Layer doesn't have a gralloc buffer", aLayer->Name());
            return false;
        }
    }

    nsIntRect visibleRect = visibleRegion.GetBounds();

    nsIntRect bufferRect;
    if (fillColor) {
        bufferRect = nsIntRect(visibleRect);
    } else {
        nsIntRect layerRect;
        if (state.mHasOwnOffset) {
            bufferRect = nsIntRect(state.mOffset.x, state.mOffset.y,
                                   state.mSize.width, state.mSize.height);
            layerRect = bufferRect;
        } else {
            //Since the buffer doesn't have its own offset, assign the whole
            //surface size as its buffer bounds
            bufferRect = nsIntRect(0, 0, state.mSize.width, state.mSize.height);
            layerRect = bufferRect;
            if (aLayer->GetType() == Layer::TYPE_IMAGE) {
                ImageLayer* imageLayer = static_cast<ImageLayer*>(aLayer);
                if(imageLayer->GetScaleMode() != ScaleMode::SCALE_NONE) {
                  layerRect = nsIntRect(0, 0, imageLayer->GetScaleToSize().width, imageLayer->GetScaleToSize().height);
                }
            }
        }
        // In some cases the visible rect assigned to the layer can be larger
        // than the layer's surface, e.g., an ImageLayer with a small Image
        // in it.
        visibleRect.IntersectRect(visibleRect, layerRect);
    }

    // Buffer rotation is not to be confused with the angled rotation done by a transform matrix
    // It's a fancy PaintedLayer feature used for scrolling
    if (state.BufferRotated()) {
        LOGD("%s Layer has a rotated buffer", aLayer->Name());
        return false;
    }

    const bool needsYFlip = state.OriginBottomLeft() ? true
                                                     : false;

    hwc_rect_t sourceCrop, displayFrame;
    if(!HwcUtils::PrepareLayerRects(visibleRect,
                          layerTransform,
                          layerBufferTransform,
                          clip,
                          bufferRect,
                          needsYFlip,
                          &(sourceCrop),
                          &(displayFrame)))
    {
        return true;
    }

    // OK!  We can compose this layer with hwc.
    int current = mList ? mList->numHwLayers : 0;

    // Do not compose any layer below full-screen Opaque layer
    // Note: It can be generalized to non-fullscreen Opaque layers.
    bool isOpaque = opacity == 0xFF &&
        (state.mFlags & LayerRenderStateFlags::OPAQUE);
    // Currently we perform opacity calculation using the *bounds* of the layer.
    // We can only make this assumption if we're not dealing with a complex visible region.
    bool isSimpleVisibleRegion = visibleRegion.Contains(visibleRect);
    if (current && isOpaque && isSimpleVisibleRegion) {
        nsIntRect displayRect = nsIntRect(displayFrame.left, displayFrame.top,
            displayFrame.right - displayFrame.left, displayFrame.bottom - displayFrame.top);
        if (displayRect.Contains(mScreenRect)) {
            // In z-order, all previous layers are below
            // the current layer. We can ignore them now.
            mList->numHwLayers = current = 0;
            mHwcLayerMap.Clear();
        }
    }

    if (!mList || current >= mMaxLayerCount) {
        if (!ReallocLayerList() || current >= mMaxLayerCount) {
            LOGE("PrepareLayerList failed! Could not increase the maximum layer count");
            return false;
        }
    }

    HwcLayer& hwcLayer = mList->hwLayers[current];
    hwcLayer.displayFrame = displayFrame;
    mHal->SetCrop(hwcLayer, sourceCrop);
    buffer_handle_t handle = nullptr;
    if (state.GetSidebandStream().IsValid()) {
        handle = state.GetSidebandStream().GetRawNativeHandle();
    } else if (state.GetGrallocBuffer()) {
        handle = state.GetGrallocBuffer()->getNativeBuffer()->handle;
    }
    hwcLayer.handle = handle;

    hwcLayer.flags = 0;
    hwcLayer.hints = 0;
    hwcLayer.blending = HWC_BLENDING_PREMULT;
    hwcLayer.compositionType = HWC_FRAMEBUFFER;
    if (state.GetSidebandStream().IsValid()) {
        hwcLayer.compositionType = HWC_SIDEBAND;
    }
    hwcLayer.acquireFenceFd = -1;
    hwcLayer.releaseFenceFd = -1;
    hwcLayer.planeAlpha = opacity;

    LayerComposite* layerComposite = static_cast<LayerComposite*>(aLayer->ImplData());
    if (layerComposite && layerComposite->Damaged()) {
        hwcLayer.surfaceDamage.numRects = 0;
        hwcLayer.surfaceDamage.rects = nullptr;
        // Because some hwc implementation's ROI checking skips video frames,
        // hwc might drop video frame in such device. To workaround this, we
        // force HWC_GEOMETRY_CHANGED to avoid ROI checking for those devices.
        if (aLayer->GetType() != Layer::TYPE_IMAGE) {
          mList->flags = HWC_GEOMETRY_CHANGED;
        }
    } else {
        static hwc_rect_t empty = {0, 0, 0, 0};
        hwcLayer.surfaceDamage.numRects = 1;
        hwcLayer.surfaceDamage.rects = &empty;
    }

    if (!fillColor) {
        if (state.FormatRBSwapped()) {
            if (!mRBSwapSupport) {
                LOGD("No R/B swap support in H/W Composer");
                return false;
            }
            hwcLayer.flags |= HwcUtils::HWC_FORMAT_RB_SWAP;
        }

        // Translation and scaling have been addressed in PrepareLayerRects().
        // Given the above and that we checked for PreservesAxisAlignedRectangles()
        // the only possible transformations left to address are
        // square angle rotation and horizontal/vertical reflection.
        //
        // The rotation and reflection permutations total 16 but can be
        // reduced to 8 transformations after eliminating redundancies.
        //
        // All matrices represented here are in the form
        //
        // | xx  xy |
        // | yx  yy |
        //
        // And ignore scaling.
        //
        // Reflection is applied before rotation
        gfx::Matrix rotation = layerTransform;
        // Compute fuzzy zero like PreservesAxisAlignedRectangles()
        if (fabs(rotation._11) < 1e-6) {
            if (rotation._21 < 0) {
                if (rotation._12 > 0) {
                    // 90 degree rotation
                    //
                    // |  0  -1  |
                    // |  1   0  |
                    //
                    hwcLayer.transform = HWC_TRANSFORM_ROT_90;
                    LOGD("Layer rotated 90 degrees");
                }
                else {
                    // Horizontal reflection then 90 degree rotation
                    //
                    // |  0  -1  | | -1   0  | = |  0  -1  |
                    // |  1   0  | |  0   1  |   | -1   0  |
                    //
                    // same as vertical reflection then 270 degree rotation
                    //
                    // |  0   1  | |  1   0  | = |  0  -1  |
                    // | -1   0  | |  0  -1  |   | -1   0  |
                    //
                    hwcLayer.transform = HWC_TRANSFORM_ROT_90 | HWC_TRANSFORM_FLIP_H;
                    LOGD("Layer vertically reflected then rotated 270 degrees");
                }
            } else {
                if (rotation._12 < 0) {
                    // 270 degree rotation
                    //
                    // |  0   1  |
                    // | -1   0  |
                    //
                    hwcLayer.transform = HWC_TRANSFORM_ROT_270;
                    LOGD("Layer rotated 270 degrees");
                }
                else {
                    // Vertical reflection then 90 degree rotation
                    //
                    // |  0   1  | | -1   0  | = |  0   1  |
                    // | -1   0  | |  0   1  |   |  1   0  |
                    //
                    // Same as horizontal reflection then 270 degree rotation
                    //
                    // |  0  -1  | |  1   0  | = |  0   1  |
                    // |  1   0  | |  0  -1  |   |  1   0  |
                    //
                    hwcLayer.transform = HWC_TRANSFORM_ROT_90 | HWC_TRANSFORM_FLIP_V;
                    LOGD("Layer horizontally reflected then rotated 270 degrees");
                }
            }
        } else if (rotation._11 < 0) {
            if (rotation._22 > 0) {
                // Horizontal reflection
                //
                // | -1   0  |
                // |  0   1  |
                //
                hwcLayer.transform = HWC_TRANSFORM_FLIP_H;
                LOGD("Layer rotated 180 degrees");
            }
            else {
                // 180 degree rotation
                //
                // | -1   0  |
                // |  0  -1  |
                //
                // Same as horizontal and vertical reflection
                //
                // | -1   0  | |  1   0  | = | -1   0  |
                // |  0   1  | |  0  -1  |   |  0  -1  |
                //
                hwcLayer.transform = HWC_TRANSFORM_ROT_180;
                LOGD("Layer rotated 180 degrees");
            }
        } else {
            if (rotation._22 < 0) {
                // Vertical reflection
                //
                // |  1   0  |
                // |  0  -1  |
                //
                hwcLayer.transform = HWC_TRANSFORM_FLIP_V;
                LOGD("Layer rotated 180 degrees");
            }
            else {
                // No rotation or reflection
                //
                // |  1   0  |
                // |  0   1  |
                //
                hwcLayer.transform = 0;
            }
        }

        const bool needsYFlip = state.OriginBottomLeft() ? true
                                                         : false;

        if (needsYFlip) {
           // Invert vertical reflection flag if it was already set
           hwcLayer.transform ^= HWC_TRANSFORM_FLIP_V;
        }
        hwc_region_t region;
        if (visibleRegion.GetNumRects() > 1) {
            mVisibleRegions.push_back(HwcUtils::RectVector());
            HwcUtils::RectVector* visibleRects = &(mVisibleRegions.back());
            bool isVisible = false;
            if(!HwcUtils::PrepareVisibleRegion(visibleRegion,
                                     layerTransform,
                                     layerBufferTransform,
                                     clip,
                                     bufferRect,
                                     visibleRects,
                                     isVisible)) {
                LOGD("A region of layer is too small to be rendered by HWC");
                return false;
            }
            if (!isVisible) {
                // Layer is not visible, no need to render it
                return true;
            }
            region.numRects = visibleRects->size();
            region.rects = &((*visibleRects)[0]);
        } else {
            region.numRects = 1;
            region.rects = &(hwcLayer.displayFrame);
        }
        hwcLayer.visibleRegionScreen = region;
    } else {
        hwcLayer.flags |= HwcUtils::HWC_COLOR_FILL;
        ColorLayer* colorLayer = aLayer->AsColorLayer();
        if (colorLayer->GetColor().a < 1.0) {
            LOGD("Color layer has semitransparency which is unsupported");
            return false;
        }
        hwcLayer.transform = colorLayer->GetColor().ToABGR();
    }

    if (aFindSidebandStreams && hwcLayer.compositionType == HWC_SIDEBAND) {
        mCachedSidebandLayers.AppendElement(hwcLayer);
    }

    mHwcLayerMap.AppendElement(static_cast<LayerComposite*>(aLayer->ImplData()));
    mList->numHwLayers++;
    return true;
#endif
}

bool HwcComposer2D::TryHwComposition(nsScreenGonk* aScreen) {
  // TODO: FIXME
  return false;
#if 0
    DisplaySurface* dispSurface = aScreen->GetDisplaySurface();

    if (!(dispSurface && dispSurface->lastHandle)) {
        LOGD("H/W Composition failed. DispSurface not initialized.");
        return false;
    }

    // Add FB layer
    int idx = mList->numHwLayers++;
    if (idx >= mMaxLayerCount) {
        if (!ReallocLayerList() || idx >= mMaxLayerCount) {
            LOGE("TryHwComposition failed! Could not add FB layer");
            return false;
        }
    }

    Prepare(dispSurface->lastHandle, -1, aScreen);

    /* Possible composition paths, after hwc prepare:
    1. GPU Composition
    2. BLIT Composition
    3. Full OVERLAY Composition
    4. Partial OVERLAY Composition (GPU + OVERLAY) */

    bool gpuComposite = false;
    bool blitComposite = false;
    bool overlayComposite = true;

    for (int j=0; j < idx; j++) {
        if (mList->hwLayers[j].compositionType == HWC_FRAMEBUFFER ||
            mList->hwLayers[j].compositionType == HWC_BLIT) {
            // Full OVERLAY composition is not possible on this frame
            // It is either GPU / BLIT / partial OVERLAY composition.
            overlayComposite = false;
            break;
        }
    }

    if (!overlayComposite) {
        for (int k=0; k < idx; k++) {
            switch (mList->hwLayers[k].compositionType) {
                case HWC_FRAMEBUFFER:
                    gpuComposite = true;
                    break;
                case HWC_BLIT:
                    blitComposite = true;
                    break;
                case HWC_SIDEBAND:
                case HWC_OVERLAY: {
                    // HWC will compose HWC_OVERLAY layers in partial
                    // Overlay Composition, set layer composition flag
                    // on mapped LayerComposite to skip GPU composition
                    mHwcLayerMap[k]->SetLayerComposited(true);

                    uint8_t opacity = std::min(0xFF, (int)(mHwcLayerMap[k]->GetLayer()->GetEffectiveOpacity() * 256.0));
                    if ((mList->hwLayers[k].hints & HWC_HINT_CLEAR_FB) &&
                        (opacity == 0xFF)) {
                        // Clear visible rect on FB with transparent pixels.
                        hwc_rect_t r = mList->hwLayers[k].displayFrame;
                        mHwcLayerMap[k]->SetClearRect(nsIntRect(r.left, r.top,
                                                                r.right - r.left,
                                                                r.bottom - r.top));
                    }
                    break;
                }
                default:
                    break;
            }
        }

        if (gpuComposite) {
            // GPU or partial OVERLAY Composition
            return false;
        } else if (blitComposite) {
            // BLIT Composition, flip DispSurface target
            RefPtr<mozilla::gl::GLContext> GLContext = aScreen->GetGLContext();
            if (GLContext) {
                // If using CompositorOGL, we need to do MakeCurrent before
                // UpdateDispSurface.
                GLContext->MakeCurrent();
            }
            GetGonkDisplay()->UpdateDispSurface(aScreen->GetEGLDisplay(),
                                                aScreen->GetEGLSurface());
            DisplaySurface* dispSurface = aScreen->GetDisplaySurface();
            if (!dispSurface) {
                LOGE("H/W Composition failed. NULL DispSurface.");
                return false;
            }
            mList->hwLayers[idx].handle = dispSurface->lastHandle;
            mList->hwLayers[idx].acquireFenceFd = dispSurface->GetPrevDispAcquireFd();
        }
    }

    // BLIT or full OVERLAY Composition
    return Commit(aScreen);
#endif
}

bool HwcComposer2D::Render(nsIWidget* aWidget) {
  nsScreenGonk* screen = static_cast<nsWindow*>(aWidget)->GetScreen();
  return GetGonkDisplay()->SwapBuffers(screen->GetDisplayType());

// TODO: FIXME
#if 0
    nsScreenGonk* screen = static_cast<nsWindow*>(aWidget)->GetScreen();

    // HWC module does not exist or mList is not created yet.
    if (!mHal->HasHwc() || !mList || !screen->IsComposer2DSupported()) {
        return GetGonkDisplay()->SwapBuffers(screen->GetDisplayType());
    } else if (!mList && !ReallocLayerList()) {
        LOGE("Cannot realloc layer list");
        return false;
    }

    DisplaySurface* dispSurface = screen->GetDisplaySurface();
    if (!dispSurface) {
        LOGE("H/W Composition failed. DispSurface not initialized.");
        return false;
    }

    if (mPrepared) {
        // No mHwc prepare, if already prepared in current draw cycle
        mList->hwLayers[mList->numHwLayers - 1].handle = dispSurface->lastHandle;
        mList->hwLayers[mList->numHwLayers - 1].acquireFenceFd = dispSurface->GetPrevDispAcquireFd();
    } else {
        // Update screen rect to handle a case that TryRenderWithHwc() is not called.
        mScreenRect = screen->GetNaturalBounds().ToUnknownRect();

        mList->flags = HWC_GEOMETRY_CHANGED;
        mList->numHwLayers = 2;
        mList->hwLayers[0].hints = 0;
        mList->hwLayers[0].compositionType = HWC_FRAMEBUFFER;
        mList->hwLayers[0].flags = HWC_SKIP_LAYER;
        mList->hwLayers[0].backgroundColor = {0};
        mList->hwLayers[0].acquireFenceFd = -1;
        mList->hwLayers[0].releaseFenceFd = -1;
        mList->hwLayers[0].displayFrame = {0, 0, mScreenRect.width, mScreenRect.height};

        // Prepare layers for sideband streams
        const uint32_t len = mCachedSidebandLayers.Length();
        for (uint32_t i = 0; i < len; ++i) {
            ++mList->numHwLayers;
            mList->hwLayers[i+1] = mCachedSidebandLayers[i];
        }
        Prepare(dispSurface->lastHandle, dispSurface->GetPrevDispAcquireFd(), screen);
    }

    // GPU or partial HWC Composition
    return Commit(screen);
#endif
}

void HwcComposer2D::Prepare(buffer_handle_t dispHandle, int fence,
                            nsScreenGonk* screen) {
  if (mPrepared) {
    LOGE("Multiple hwc prepare calls!");
  }
  hwc_rect_t dispRect = {0, 0, mScreenRect.width, mScreenRect.height};
  mHal->Prepare(mList, (uint32_t)screen->GetDisplayType(), dispRect, dispHandle,
                fence);
  mPrepared = true;
}

bool HwcComposer2D::Commit(nsScreenGonk* aScreen) {
  // TODO: FIXME
  return false;
#if 0
    for (uint32_t j=0; j < (mList->numHwLayers - 1); j++) {
        mList->hwLayers[j].acquireFenceFd = -1;
        if (mHwcLayerMap.IsEmpty() ||
            (mList->hwLayers[j].compositionType == HWC_FRAMEBUFFER)) {
            continue;
        }
        LayerRenderState state = mHwcLayerMap[j]->GetLayer()->GetRenderState();
        if (!state.mTexture) {
            continue;
        }
        FenceHandle fence = state.mTexture->GetAndResetAcquireFenceHandle();
        if (fence.IsValid()) {
            RefPtr<FenceHandle::FdObj> fdObj = fence.GetAndResetFdObj();
            mList->hwLayers[j].acquireFenceFd = fdObj->GetAndResetFd();
        }
    }

    int err = mHal->Set(mList, aScreen->GetDisplayType());

    mPrevRetireFence.TransferToAnotherFenceHandle(mPrevDisplayFence);

    for (uint32_t j=0; j < (mList->numHwLayers - 1); j++) {
        if (mList->hwLayers[j].releaseFenceFd >= 0) {
            int fd = mList->hwLayers[j].releaseFenceFd;
            mList->hwLayers[j].releaseFenceFd = -1;
            RefPtr<FenceHandle::FdObj> fdObj = new FenceHandle::FdObj(fd);
            FenceHandle fence(fdObj);

            LayerRenderState state = mHwcLayerMap[j]->GetLayer()->GetRenderState();
            if (!state.mTexture) {
                continue;
            }
            state.mTexture->SetReleaseFenceHandle(fence);
        }
    }

    if (mList->retireFenceFd >= 0) {
        mPrevRetireFence = FenceHandle(new FenceHandle::FdObj(mList->retireFenceFd));
    }

    // Set DisplaySurface layer fence
    DisplaySurface* displaySurface = aScreen->GetDisplaySurface();
    displaySurface->setReleaseFenceFd(mList->hwLayers[mList->numHwLayers - 1].releaseFenceFd);
    mList->hwLayers[mList->numHwLayers - 1].releaseFenceFd = -1;

    mPrepared = false;
    return !err;
#endif
}

bool HwcComposer2D::TryRenderWithHwc(Layer* aRoot, nsIWidget* aWidget,
                                     bool aGeometryChanged,
                                     bool aHasImageHostOverlays) {
  // TODO: FIXME
  return false;
#if 0
    if (!mHal->HasHwc()) {
        return false;
    }

    nsScreenGonk* screen = static_cast<nsWindow*>(aWidget)->GetScreen();

    // On certain devices, ex: Octans, hwc only controls primary screen.
    // For exteranl screen we should return false and fall back to GPU
    // composition.
    if (!screen->IsComposer2DSupported()) {
        return false;
    }

    if (mStopRenderWithHwc) {
        return false;
    }

    if (mList) {
        mList->flags = mHal->GetGeometryChangedFlag(aGeometryChanged);
        mList->numHwLayers = 0;
        mHwcLayerMap.Clear();
    }

    if (mPrepared) {
        mHal->ResetHwc();
        mPrepared = false;
    }

    // XXX: The clear() below means all rect vectors will be have to be
    // reallocated. We may want to avoid this if possible
    mVisibleRegions.clear();

    mScreenRect = screen->GetNaturalBounds().ToUnknownRect();
    MOZ_ASSERT(mHwcLayerMap.IsEmpty());
    mCachedSidebandLayers.Clear();
    if (!PrepareLayerList(aRoot,
                          mScreenRect,
                          gfx::Matrix(),
                          /* aFindSidebandStreams */ false))
    {
        mHwcLayerMap.Clear();
        LOGD("Render aborted. Fallback to GPU Composition");
        if (aHasImageHostOverlays) {
            LOGD("Prepare layers of SidebandStreams");
            // Failed to create a layer list for hwc. But we need the list
            // only for handling sideband streams. Traverse layer tree without
            // some early returns to make sure we can find all the layers.
            // It is the best wrong thing that we can do.
            PrepareLayerList(aRoot,
                             mScreenRect,
                             gfx::Matrix(),
                             /* aFindSidebandStreams */ true);
            // Reset mPrepared to false, since we already fell back to
            // gpu composition.
            mPrepared = false;
        }
        return false;
    }

    // Send data to LayerScope for debugging
    SendtoLayerScope();

    if (!TryHwComposition(screen)) {
        LOGD("Full HWC Composition failed. Fallback to GPU Composition or partial OVERLAY Composition");
// TODO: FIXME
#  if 0
        LayerScope::CleanLayer();
#  endif
        return false;
    }

    LOGD("Frame rendered");
    return true;
#endif
}

void HwcComposer2D::SendtoLayerScope() {
// TODO: FIXME
#if 0
    if (!LayerScope::CheckSendable()) {
        return;
    }

    const int len = mList->numHwLayers;
    for (int i = 0; i < len; ++i) {
        LayerComposite* layer = mHwcLayerMap[i];
        const hwc_rect_t r = mList->hwLayers[i].displayFrame;
        LayerScope::SendLayer(layer, r.right - r.left, r.bottom - r.top);
    }
#endif
}

void HwcComposer2D::StopRenderWithHwc(bool aIsStop) {
  mStopRenderWithHwc = aIsStop;
}

void HwcComposer2D::SetVsyncAlwaysEnabled(bool aAlways) {
  mAlwaysEnabled = aAlways;
}

}  // namespace mozilla
