#ifndef DISPLAY_LIST_TESTING_DL_RENDER_TEST_ENV_H_
#define DISPLAY_LIST_TESTING_DL_RENDER_TEST_ENV_H_

#include "display_list/testing/dl_render_test.h"

#include "include/core/SkRefCnt.h"
#include "include/core/SkSurface.h"

namespace flutter {

class DisplayList;

namespace testing {

// Test render surface dimensions. Mirrors the Flutter test convention of
// rendering inside a 200x200 surface.
inline constexpr int kTestWidth = 200;
inline constexpr int kTestHeight = 200;

// Records (op + variation) into a DisplayList, then dispatches it through
// every registered backend. First phase ships a single backend (Skia raster);
// future backends (e.g. CALayer) plug in by adding another dispatcher path.
class RenderEnv {
 public:
  RenderEnv();

  // Build a DisplayList from `setup` then `op` and dispatch it to the Skia
  // raster backend. Returns the rendered surface.
  sk_sp<SkSurface> RenderToSkia(const VariationCase& variation,
                                const OpCase& op);

  // Smoke check: any pixel in the surface differs from the cleared (0x0)
  // background. Cheaper than full image diff but enough to catch a backend
  // that silently no-ops a draw.
  static bool HasNonClearPixel(const sk_sp<SkSurface>& surface);
};

}  // namespace testing
}  // namespace flutter

#endif  // DISPLAY_LIST_TESTING_DL_RENDER_TEST_ENV_H_
