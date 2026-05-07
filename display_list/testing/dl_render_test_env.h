#ifndef DISPLAY_LIST_TESTING_DL_RENDER_TEST_ENV_H_
#define DISPLAY_LIST_TESTING_DL_RENDER_TEST_ENV_H_

#include <cstdint>
#include <string>

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

// Builds a DisplayList from (variation × op) and dispatches it to one or more
// rendering backends. Each backend renders into an N32Premul SkSurface so the
// resulting pixel buffers can be compared byte-for-byte.
class RenderEnv {
 public:
  RenderEnv();

  // Build a DisplayList from `setup` then `op` and dispatch it to the Skia
  // raster backend. This is the comparison baseline — every other backend's
  // output is checked against this surface.
  sk_sp<SkSurface> RenderToSkia(const VariationCase& variation,
                                const OpCase& op);

  // Build the same DisplayList and dispatch it through the CALayer backend.
  sk_sp<SkSurface> RenderToCALayer(const VariationCase& variation,
                                   const OpCase& op);

  // Compare two rasterized surfaces channel-by-channel. Returns true iff
  // every BGRA byte differs by at most `max_per_channel_diff`. On mismatch,
  // populates `mismatch_message` with a human-readable description of the
  // first offending pixel — caller passes this to GTest's failure output.
  static bool CompareSurfaces(const sk_sp<SkSurface>& expected,
                              const sk_sp<SkSurface>& actual,
                              uint8_t max_per_channel_diff,
                              std::string* mismatch_message);
};

}  // namespace testing
}  // namespace flutter

#endif  // DISPLAY_LIST_TESTING_DL_RENDER_TEST_ENV_H_
