#include "display_list/testing/dl_render_test_env.h"

#include <cstdio>
#include <cstdlib>

#include "display_list/display_list.h"
#include "display_list/dl_builder.h"
#include "display_list/geometry/dl_geometry_types.h"
#include "display_list/skia/dl_sk_dispatcher.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"

#include <CoreGraphics/CoreGraphics.h>

#include "display_list/calayer/dl_ca_dispatcher.h"

namespace flutter {
namespace testing {

namespace {

// Build the DisplayList that both backends will render. Centralised so the
// per-backend Render* methods stay in lockstep.
sk_sp<DisplayList> BuildDisplayList(const VariationCase& variation,
                                    const OpCase& op) {
  DisplayListBuilder builder(DlRect::MakeWH(kTestWidth, kTestHeight));
  DlPaint paint;
  RenderContext ctx{&builder, paint};
  variation.setup(ctx);
  op.op(ctx);
  return builder.Build();
}

sk_sp<SkSurface> MakeRasterSurface() {
  sk_sp<SkSurface> surface =
      SkSurfaces::Raster(SkImageInfo::MakeN32Premul(kTestWidth, kTestHeight));
  surface->getCanvas()->clear(SK_ColorTRANSPARENT);
  return surface;
}

}  // namespace

RenderEnv::RenderEnv() = default;

sk_sp<SkSurface> RenderEnv::RenderToSkia(const VariationCase& variation,
                                         const OpCase& op) {
  sk_sp<DisplayList> dl = BuildDisplayList(variation, op);
  sk_sp<SkSurface> surface = MakeRasterSurface();
  DlSkCanvasDispatcher dispatcher(surface->getCanvas());
  dl->Dispatch(dispatcher);
  return surface;
}

sk_sp<SkSurface> RenderEnv::RenderToCALayer(const VariationCase& variation,
                                            const OpCase& op) {
  sk_sp<DisplayList> dl = BuildDisplayList(variation, op);

  // Allocate a 32-bit BGRA premultiplied buffer that we can wrap as both an
  // SkSurface (for comparison) and a CGBitmapContext (for CALayer to draw
  // into). MakeN32Premul on Apple resolves to kBGRA_8888_SkColorType, which
  // matches kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little.
  sk_sp<SkSurface> surface = MakeRasterSurface();

  SkPixmap pm;
  if (!surface->peekPixels(&pm)) {
    return nullptr;
  }

  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  // CG bit flags come from two anonymous enums; OR-combining them through
  // uint32_t avoids the C++20 deprecation warning that bans bit ops between
  // distinct enum types.
  uint32_t bitmap_info =
      static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst) |
      static_cast<uint32_t>(kCGBitmapByteOrder32Little);
  CGContextRef ctx = CGBitmapContextCreate(
      pm.writable_addr(), pm.width(), pm.height(), /*bitsPerComponent=*/8,
      pm.rowBytes(), cs, bitmap_info);
  CGColorSpaceRelease(cs);
  if (!ctx) {
    return nullptr;
  }

  DlCALayerDispatcher dispatcher(kTestWidth, kTestHeight);
  dl->Dispatch(dispatcher);
  dispatcher.RenderInContext(ctx);

  CGContextRelease(ctx);
  surface->notifyContentWillChange(SkSurface::kRetain_ContentChangeMode);
  return surface;
}

bool RenderEnv::CompareSurfaces(const sk_sp<SkSurface>& expected,
                                const sk_sp<SkSurface>& actual,
                                uint8_t max_per_channel_diff,
                                std::string* mismatch_message) {
  SkPixmap exp_pm;
  SkPixmap act_pm;
  if (!expected || !expected->peekPixels(&exp_pm)) {
    if (mismatch_message) {
      *mismatch_message = "expected surface has no readable pixels";
    }
    return false;
  }
  if (!actual || !actual->peekPixels(&act_pm)) {
    if (mismatch_message) {
      *mismatch_message = "actual surface has no readable pixels";
    }
    return false;
  }
  if (exp_pm.width() != act_pm.width() || exp_pm.height() != act_pm.height()) {
    if (mismatch_message) {
      char buf[128];
      std::snprintf(
          buf, sizeof(buf), "size mismatch: expected %dx%d, actual %dx%d",
          exp_pm.width(), exp_pm.height(), act_pm.width(), act_pm.height());
      *mismatch_message = buf;
    }
    return false;
  }

  for (int y = 0; y < exp_pm.height(); ++y) {
    const uint8_t* erow = static_cast<const uint8_t*>(exp_pm.addr(0, y));
    const uint8_t* arow = static_cast<const uint8_t*>(act_pm.addr(0, y));
    for (int x = 0; x < exp_pm.width(); ++x) {
      for (int c = 0; c < 4; ++c) {
        int e = erow[x * 4 + c];
        int a = arow[x * 4 + c];
        if (std::abs(e - a) > max_per_channel_diff) {
          if (mismatch_message) {
            char buf[256];
            std::snprintf(
                buf, sizeof(buf),
                "pixel mismatch at (%d, %d) channel %d: expected 0x%02x, "
                "actual 0x%02x (diff %d > tol %d)",
                x, y, c, e, a, std::abs(e - a),
                static_cast<int>(max_per_channel_diff));
            *mismatch_message = buf;
          }
          return false;
        }
      }
    }
  }
  return true;
}

}  // namespace testing
}  // namespace flutter
