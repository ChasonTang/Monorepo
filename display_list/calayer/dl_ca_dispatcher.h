// CALayer-based DlOpReceiver. Recorded display lists are translated into a
// CALayer tree at dispatch time, then rasterized via -[CALayer renderInContext:]
// into a caller-provided CGBitmapContext.
//
// The same source compiles unchanged on iOS — QuartzCore / CoreGraphics are
// identical on both platforms. The current toolchain only targets macOS, so
// the render-test executable runs there.

#ifndef DISPLAY_LIST_CALAYER_DL_CA_DISPATCHER_H_
#define DISPLAY_LIST_CALAYER_DL_CA_DISPATCHER_H_

#include <CoreGraphics/CoreGraphics.h>

#include <memory>

#include "display_list/dl_color.h"
#include "display_list/dl_op_receiver.h"
#include "display_list/utils/dl_receiver_utils.h"

namespace flutter {

// DlOpReceiver that constructs a CALayer hierarchy from a DisplayList and
// rasterizes it via -[CALayer renderInContext:]. Only the operations exercised
// by the current render-test suite (drawColor, drawPaint, save/restore) are
// implemented; the rest inherit no-op stubs from the IgnoreXxxDispatchHelper
// utilities. Adding more ops is a matter of overriding their methods and
// extending the layer tree.
class DlCALayerDispatcher : public virtual DlOpReceiver,
                            public IgnoreAttributeDispatchHelper,
                            public IgnoreClipDispatchHelper,
                            public IgnoreTransformDispatchHelper,
                            public IgnoreDrawDispatchHelper {
 public:
  DlCALayerDispatcher(int width, int height);
  // DlOpReceiver doesn't declare a virtual destructor; the dispatcher is
  // owned by-value at the test call site, so a non-virtual ~Foo is correct.
  ~DlCALayerDispatcher();

  // Render the accumulated CALayer tree into `ctx`. The context is expected
  // to be a CGBitmapContext sized at least (width, height) created with the
  // same byte order as Skia's N32Premul (BGRA premultiplied) so that the
  // rendered pixels can be compared directly.
  void RenderInContext(CGContextRef ctx);

  // Attribute setters we care about — others are no-op'd by
  // IgnoreAttributeDispatchHelper.
  void setColor(DlColor color) override;
  void setBlendMode(DlBlendMode mode) override;

  // Save / restore manage the parent layer used for subsequent draws.
  void save() override;
  void restore() override;

  // Draw ops we care about — others are no-op'd by IgnoreDrawDispatchHelper.
  void drawPaint() override;
  void drawColor(DlColor color, DlBlendMode mode) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  int width_;
  int height_;

  // Accumulated paint state. Only color is used by the currently-supported
  // draw ops; expand as needed.
  DlColor current_color_ = DlColor::kBlack();
  DlBlendMode current_blend_mode_ = DlBlendMode::kSrcOver;
};

}  // namespace flutter

#endif  // DISPLAY_LIST_CALAYER_DL_CA_DISPATCHER_H_
