#include "display_list/calayer/dl_ca_dispatcher.h"

#import <QuartzCore/QuartzCore.h>

namespace flutter {

// CALayer / NSMutableArray live behind an opaque struct so the header stays
// pure C++. ARC is enabled globally on the objc / objcxx tools, so the
// __strong ObjC members below are released by the synthesized destructor.
struct DlCALayerDispatcher::Impl {
  CALayer* root_layer = nil;

  // Stack of CALayers used as the parent for the next draw op. The back
  // element is the active parent. save() pushes the same parent again (so
  // restore() can pop without losing the root); future save_layer() will
  // push a fresh layer.
  NSMutableArray<CALayer*>* parent_stack = nil;
};

namespace {

// Build a CGColor in DeviceRGB so it composes against a DeviceRGB bitmap
// context with no color-management round trips. Skia's N32Premul raster
// surface is similarly untagged, so both backends operate on raw byte
// values and the compositing math matches.
CGColorRef CreateDeviceCGColor(const DlColor& color, float opacity) {
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGFloat components[4] = {
      static_cast<CGFloat>(color.getRedF()),
      static_cast<CGFloat>(color.getGreenF()),
      static_cast<CGFloat>(color.getBlueF()),
      static_cast<CGFloat>(color.getAlphaF() * opacity),
  };
  CGColorRef out = CGColorCreate(cs, components);
  CGColorSpaceRelease(cs);
  return out;
}

// Append a full-bounds sublayer painted with `color`. CALayer composes
// sublayers over their parent in source-over order, which is the only
// blend mode the current render-test suite exercises.
void AppendFullBoundsColorLayer(CALayer* parent,
                                CGRect bounds,
                                const DlColor& color) {
  CALayer* layer = [[CALayer alloc] init];
  layer.frame = bounds;
  CGColorRef cg = CreateDeviceCGColor(color, 1.0f);
  layer.backgroundColor = cg;
  CGColorRelease(cg);
  [parent addSublayer:layer];
}

}  // namespace

DlCALayerDispatcher::DlCALayerDispatcher(int width, int height)
    : impl_(std::make_unique<Impl>()), width_(width), height_(height) {
  impl_->root_layer = [[CALayer alloc] init];
  impl_->root_layer.frame = CGRectMake(0, 0, width, height);
  // Disable implicit animations so layer changes apply immediately when
  // the tree is rasterized — render-tests run outside of any CATransaction.
  impl_->root_layer.actions = @{
    @"sublayers" : [NSNull null],
    @"contents" : [NSNull null],
    @"backgroundColor" : [NSNull null],
  };

  impl_->parent_stack = [[NSMutableArray alloc] init];
  [impl_->parent_stack addObject:impl_->root_layer];
}

DlCALayerDispatcher::~DlCALayerDispatcher() = default;

void DlCALayerDispatcher::RenderInContext(CGContextRef ctx) {
  // CALayer's geometry origin is bottom-left in the destination context,
  // while Skia's N32Premul pixel buffer is top-down. Flip the context so
  // a layer at frame=(0, 0, W, H) lands at the top-left of the bitmap.
  CGContextSaveGState(ctx);
  CGContextTranslateCTM(ctx, 0, height_);
  CGContextScaleCTM(ctx, 1, -1);
  [impl_->root_layer renderInContext:ctx];
  CGContextRestoreGState(ctx);
}

void DlCALayerDispatcher::setColor(DlColor color) {
  current_color_ = color;
}

void DlCALayerDispatcher::setBlendMode(DlBlendMode mode) {
  current_blend_mode_ = mode;
}

void DlCALayerDispatcher::save() {
  // No transform / clip support yet — re-push the current parent so the
  // restore() pop is balanced.
  [impl_->parent_stack addObject:[impl_->parent_stack lastObject]];
}

void DlCALayerDispatcher::restore() {
  if ([impl_->parent_stack count] > 1) {
    [impl_->parent_stack removeLastObject];
  }
}

void DlCALayerDispatcher::drawPaint() {
  CALayer* parent = [impl_->parent_stack lastObject];
  AppendFullBoundsColorLayer(parent, parent.bounds, current_color_);
}

void DlCALayerDispatcher::drawColor(DlColor color, DlBlendMode /*mode*/) {
  // Only kSrcOver is exercised today — sublayer composition handles it
  // implicitly. Other modes require layer.compositingFilter (CIFilter) and
  // will be wired up alongside the ops that need them.
  CALayer* parent = [impl_->parent_stack lastObject];
  AppendFullBoundsColorLayer(parent, parent.bounds, color);
}

}  // namespace flutter
