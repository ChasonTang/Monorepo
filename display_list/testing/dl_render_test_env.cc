#include "display_list/testing/dl_render_test_env.h"

#include "display_list/display_list.h"
#include "display_list/dl_builder.h"
#include "display_list/geometry/dl_geometry_types.h"
#include "display_list/skia/dl_sk_dispatcher.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"

namespace flutter {
namespace testing {

RenderEnv::RenderEnv() = default;

sk_sp<SkSurface> RenderEnv::RenderToSkia(const VariationCase& variation,
                                         const OpCase& op) {
  DisplayListBuilder builder(DlRect::MakeWH(kTestWidth, kTestHeight));
  DlPaint paint;
  RenderContext ctx{&builder, paint};
  variation.setup(ctx);
  op.op(ctx);
  sk_sp<DisplayList> dl = builder.Build();

  sk_sp<SkSurface> surface = SkSurfaces::Raster(
      SkImageInfo::MakeN32Premul(kTestWidth, kTestHeight));
  surface->getCanvas()->clear(SK_ColorTRANSPARENT);

  DlSkCanvasDispatcher dispatcher(surface->getCanvas());
  dl->Dispatch(dispatcher);
  return surface;
}

bool RenderEnv::HasNonClearPixel(const sk_sp<SkSurface>& surface) {
  SkPixmap pm;
  if (!surface->peekPixels(&pm)) {
    return false;
  }
  for (int y = 0; y < pm.height(); ++y) {
    const uint32_t* row = pm.addr32(0, y);
    for (int x = 0; x < pm.width(); ++x) {
      if (row[x] != 0u) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace testing
}  // namespace flutter
