/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "gm.h"
#include "Resources.h"
#include "SkCodec.h"
#include "SkColorSpace.h"
#include "SkColorSpace_Base.h"
#include "SkHalf.h"
#include "SkImage.h"

static void clamp_if_necessary(const SkImageInfo& info, void* pixels) {
    if (kRGBA_F16_SkColorType != info.colorType()) {
        return;
    }

    for (int y = 0; y < info.height(); y++) {
        for (int x = 0; x < info.width(); x++) {
            uint64_t pixel = ((uint64_t*) pixels)[y * info.width() + x];

            Sk4f rgba = SkHalfToFloat_finite_ftz(pixel);
            if (kUnpremul_SkAlphaType == info.alphaType()) {
                rgba = Sk4f::Max(0.0f, Sk4f::Min(rgba, 1.0f));
            } else {
                SkASSERT(kPremul_SkAlphaType == info.alphaType());
                rgba = Sk4f::Max(0.0f, Sk4f::Min(rgba, rgba[3]));
            }
            SkFloatToHalf_finite_ftz(rgba).store(&pixel);

            ((uint64_t*) pixels)[y * info.width() + x] = pixel;
        }
    }
}

sk_sp<SkColorSpace> fix_for_colortype(SkColorSpace* colorSpace, SkColorType colorType) {
    if (kRGBA_F16_SkColorType == colorType) {
        return as_CSB(colorSpace)->makeLinearGamma();
    }

    return sk_ref_sp(colorSpace);
}

static const int kWidth = 64;
static const int kHeight = 64;

static sk_sp<SkImage> make_raster_image(SkColorType colorType, SkAlphaType alphaType) {
    std::unique_ptr<SkStream> stream(GetResourceAsStream("google_chrome.ico"));
    std::unique_ptr<SkCodec> codec(SkCodec::NewFromStream(stream.release()));

    SkBitmap bitmap;
    SkImageInfo info = codec->getInfo().makeWH(kWidth, kHeight)
                                       .makeColorType(colorType)
                                       .makeAlphaType(alphaType)
            .makeColorSpace(fix_for_colortype(codec->getInfo().colorSpace(), colorType));
    bitmap.allocPixels(info);
    codec->getPixels(info, bitmap.getPixels(), bitmap.rowBytes());
    bitmap.setImmutable();
    return SkImage::MakeFromBitmap(bitmap);
}

static sk_sp<SkColorSpace> make_srgb_transfer_fn(const SkColorSpacePrimaries& primaries) {
    SkMatrix44 toXYZD50(SkMatrix44::kUninitialized_Constructor);
    SkAssertResult(primaries.toXYZD50(&toXYZD50));
    return SkColorSpace::MakeRGB(SkColorSpace::kSRGB_RenderTargetGamma, toXYZD50);
}

static sk_sp<SkColorSpace> make_wide_gamut() {
    // ProPhoto
    SkColorSpacePrimaries primaries;
    primaries.fRX = 0.7347f;
    primaries.fRY = 0.2653f;
    primaries.fGX = 0.1596f;
    primaries.fGY = 0.8404f;
    primaries.fBX = 0.0366f;
    primaries.fBY = 0.0001f;
    primaries.fWX = 0.34567f;
    primaries.fWY = 0.35850f;
    return make_srgb_transfer_fn(primaries);
}

static sk_sp<SkColorSpace> make_small_gamut() {
    SkColorSpacePrimaries primaries;
    primaries.fRX = 0.50f;
    primaries.fRY = 0.33f;
    primaries.fGX = 0.30f;
    primaries.fGY = 0.50f;
    primaries.fBX = 0.25f;
    primaries.fBY = 0.16f;
    primaries.fWX = 0.3127f;
    primaries.fWY = 0.3290f;
    return make_srgb_transfer_fn(primaries);
}

static void draw_image(SkCanvas* canvas, SkImage* image, SkColorType dstColorType,
                       SkAlphaType dstAlphaType, sk_sp<SkColorSpace> dstColorSpace) {
    size_t rowBytes = image->width() * SkColorTypeBytesPerPixel(dstColorType);
    sk_sp<SkData> data = SkData::MakeUninitialized(rowBytes * image->height());
    dstColorSpace = fix_for_colortype(dstColorSpace.get(), dstColorType);
    SkImageInfo dstInfo = SkImageInfo::Make(image->width(), image->height(), dstColorType,
                                            dstAlphaType, dstColorSpace);
    image->readPixels(dstInfo, data->writable_data(), rowBytes, 0, 0);

    // readPixels() does not always clamp F16.  The drawing code expects pixels in the 0-1 range.
    clamp_if_necessary(dstInfo, data->writable_data());

    // Now that we have called readPixels(), dump the raw pixels into an srgb image.
    sk_sp<SkColorSpace> srgb = fix_for_colortype(
            SkColorSpace::MakeNamed(SkColorSpace::kSRGB_Named).get(), dstColorType);
    sk_sp<SkImage> raw = SkImage::MakeRasterData(dstInfo.makeColorSpace(srgb), data, rowBytes);
    canvas->drawImage(raw.get(), 0.0f, 0.0f, nullptr);
}

class ReadPixelsGM : public skiagm::GM {
public:
    ReadPixelsGM() {}

protected:
    SkString onShortName() override {
        return SkString("readpixels");
    }

    SkISize onISize() override {
        return SkISize::Make(6 * kWidth, 18 * kHeight);
    }

    void onDraw(SkCanvas* canvas) override {
        if (!canvas->imageInfo().colorSpace()) {
            // This gm is only interesting in color correct modes.
            return;
        }

        const SkAlphaType alphaTypes[] = {
                kUnpremul_SkAlphaType,
                kPremul_SkAlphaType,
        };
        const SkColorType colorTypes[] = {
                kRGBA_8888_SkColorType,
                kBGRA_8888_SkColorType,
                kRGBA_F16_SkColorType,
        };
        const sk_sp<SkColorSpace> colorSpaces[] = {
                make_wide_gamut(),
                SkColorSpace::MakeNamed(SkColorSpace::kSRGB_Named),
                make_small_gamut(),
        };

        for (sk_sp<SkColorSpace> dstColorSpace : colorSpaces) {
            for (SkColorType srcColorType : colorTypes) {
                for (SkAlphaType srcAlphaType : alphaTypes) {
                    canvas->save();
                    sk_sp<SkImage> image = make_raster_image(srcColorType, srcAlphaType);
                    for (SkColorType dstColorType : colorTypes) {
                        for (SkAlphaType dstAlphaType : alphaTypes) {
                            draw_image(canvas, image.get(), dstColorType, dstAlphaType,
                                       dstColorSpace);
                            canvas->translate((float) kWidth, 0.0f);
                        }
                    }
                    canvas->restore();
                    canvas->translate(0.0f, (float) kHeight);
                }
            }
        }
    }

private:
    typedef skiagm::GM INHERITED;
};
DEF_GM( return new ReadPixelsGM; )
