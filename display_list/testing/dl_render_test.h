#ifndef DISPLAY_LIST_TESTING_DL_RENDER_TEST_H_
#define DISPLAY_LIST_TESTING_DL_RENDER_TEST_H_

#include <functional>
#include <string>

#include "display_list/dl_canvas.h"
#include "display_list/dl_paint.h"

namespace flutter {
namespace testing {

// Passed to every Variation setup and Op draw lambda. `canvas` is the
// DisplayListBuilder being recorded into; `paint` is a fresh DlPaint that
// Variations may mutate before the Op draws.
struct RenderContext {
  DlCanvas* canvas;
  DlPaint& paint;
};

// One drawing operation under test (e.g. DrawPaint, DrawColor).
struct OpCase {
  std::string name;
  std::function<void(const RenderContext&)> op;
};

// One pre-draw setup variation (e.g. Defaults, RenderWithTransforms).
// Runs before the Op; may mutate canvas state and/or paint.
struct VariationCase {
  std::string name;
  std::function<void(const RenderContext&)> setup;
};

}  // namespace testing
}  // namespace flutter

#endif  // DISPLAY_LIST_TESTING_DL_RENDER_TEST_H_
