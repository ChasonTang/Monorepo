# RFC-001: SkDevice CALayer Rendering Backend for iOS

**Version:** 1.5
**Author:** Chason Tang
**Date:** 2026-03-20
**Status:** Proposed

---

## 1. Summary

This proposal introduces a new Skia rendering backend that translates SkCanvas drawing operations into a Core Animation CALayer tree on iOS. Instead of rasterizing pixels, each draw call (e.g., `drawRect`) creates or mutates a corresponding `CALayer` (or `CAShapeLayer`) and inserts it into a layer hierarchy. The approach is incremental: we start with `drawRect` as the first implemented operation, with all other SkCanvas virtual hooks stubbed as no-ops. A test-first methodology ensures correctness from day one, using Skia's existing DM/GM/unit-test infrastructure.

## 2. Motivation

Today, Skia renders on iOS by rasterizing into a pixel buffer (via the CPU raster backend or a GPU backend like Metal/Ganesh), then presenting that buffer through a `CALayer`. This works well for complex 2D scenes, but the application ends up with a flat bitmap that discards all structural information. A CALayer-tree backend preserves that structure, unlocking several Core Animation advantages:

- **Compositor-driven animation.** CALayer properties (position, bounds, transform, opacity) are animated by the window server on a dedicated render thread, producing 60/120 fps transitions without blocking the main thread. Skia-rasterized content requires re-rendering every frame.
- **Hit testing (CALayer-level, not SkCanvas-level).** `CALayer`'s built-in `hitTest:` and `containsPoint:` methods provide automatic, spatial hit testing against the exact layer geometry. Note that SkCanvas does not expose a hit-testing API — the caller works with the output CALayer tree directly — but a flat pixel buffer provides no geometry to query at all.
- **Accessibility and system integration (CALayer-level, not SkCanvas-level).** The iOS accessibility subsystem introspects the layer tree for `UIAccessibilityElement` mapping. As with hit testing, this is a property of the output layer tree, not the SkCanvas API; a flat pixel buffer provides no structural information to introspect.
- **Memory and CPU efficiency via multi-process compositing.** On iOS, Core Animation operates across two processes: the application process maintains only a lightweight layer-tree description, while the render server (`backboardd`) owns the actual framebuffers and drives the display-link timer. With a rasterized backend, the application must allocate a full-resolution pixel buffer in its own address space and re-render each frame; with a CALayer tree, the application holds only the structural description, and all pixel work happens out-of-process.

By implementing a CALayer-backed SkDevice, applications can choose this backend for UI-oriented rendering while continuing to use the raster or GPU backends for complex content — enabling a hybrid rendering strategy.

## 3. Goals and Non-Goals

**Goals:**
- Implement `SkCALayerDevice`, a new `SkDevice` subclass that emits a `CALayer` tree for each drawing operation.
- Provide incremental SkCanvas coverage: `drawRect` is implemented first; all other draw hooks are no-op stubs that compile and link but produce no visual output.
- Establish a test harness using Skia's existing DM, GM, and unit-test infrastructure, with tests written before implementation (red-green cycle).
- Define a public API to extract the resulting `CALayer` root from the device after drawing completes.

**Non-Goals:**
- Full SkCanvas API coverage in this proposal — complex operations (text, images, shaders, blend modes, clip paths) are deferred to subsequent RFCs.
- Performance parity with the raster or GPU backends — the CALayer backend targets UI-style content, not complex 2D rendering workloads.
- Android or other non-Apple platform support — this backend is iOS-only (and macOS via Catalyst/AppKit in the future, but not in scope here).
- **`saveLayer()` compositing** — `createDevice()` returns `nullptr`. When this happens, `SkCanvas::internalSaveLayer()` creates an `SkNoPixelsDevice` as a fallback — all subsequent draw calls within that `saveLayer()` scope are silently discarded (the `SkNoPixelsDevice` draw methods are empty no-ops), and during `restore()`, Skia skips compositing the layer back (via the `isNoPixelsDevice()` check). The save/restore stack still functions (save count increments, clip and transform state is preserved), but no drawing output is produced for the affected scope. Callers must avoid relying on `saveLayer()` for correct rendering in Phase 2. This limitation is documented in the virtual function table (Section 4.2.2) and will be addressed in a future RFC (see Section 9, "Save-layer compositing").

## 4. Design

### 4.1 Overview

The backend sits at the **SkDevice** layer, not the SkCanvas layer. This follows the established Skia pattern where SkCanvas handles state management (save/restore, clip stack, transforms, image filters) and delegates draw calls to the currently active SkDevice.

```
                                  ┌──────────────────────────┐
                                  │        SkCanvas          │
                                  │  (state mgmt, clip,      │
                                  │   transforms, filters)   │
                                  └────────────┬─────────────┘
                                               │
                              drawRect(r, paint)│
                                               │ delegates to
                                               ▼
                          ┌──────────────────────────────────────┐
                          │         SkCALayerDevice               │
                          │  (extends SkClipStackDevice)          │
                          │                                      │
                          │  drawRect(r, paint) {                │
                          │    - map r through localToDevice()   │
                          │    - create CAShapeLayer              │
                          │    - set path = rect                 │
                          │    - apply paint → fill/stroke/color │
                          │    - add to fRootLayer               │
                          │  }                                   │
                          │                                      │
                          │  fRootLayer (caller-owned CALayer*)   │
                          │    → sublayers accumulate here       │
                          └──────────────────────────────────────┘
                                               │
                                               │ produces
                                               ▼
                          ┌──────────────────────────────────────┐
                          │           CALayer (root)              │
                          │  ├── CAShapeLayer (rect 1)           │
                          │  ├── CAShapeLayer (rect 2)           │
                          │  └── ...                             │
                          └──────────────────────────────────────┘
```

**Why SkDevice and not SkCanvas?** Skia's architecture cleanly separates canvas-level state management from device-level rendering. Every existing backend (raster, GPU/Ganesh, GPU/Graphite, PDF, SVG, XPS) follows this pattern. Inheriting from `SkCanvasVirtualEnforcer<SkCanvas>` or `SkNoDrawCanvas` would bypass the canvas's built-in clip, transform, and image-filter machinery, requiring us to reimplement that logic. By working at the SkDevice level, we inherit all of that for free.

### 4.2 Detailed Design

#### 4.2.1 Class Hierarchy and Base Class Choice

```
SkDevice
  └── SkClipStackDevice          ← manages SkClipStack
        └── SkCALayerDevice      ← our new class (this RFC)
```

We extend `SkClipStackDevice` (the same base used by `SkSVGDevice`, `SkPDFDevice`, and `SkXPSDevice`) because:

1. **Clip state tracking.** `SkClipStackDevice` provides a fully implemented `SkClipStack` with `pushClipStack()`, `popClipStack()`, `clipRect()`, `clipRRect()`, `clipPath()`, `clipRegion()`, `replaceClip()`, and all `isClip*()` queries. We inherit clip state tracking without writing a single line of clip code. Note: in Phase 2 the device only *tracks* clip state — it does **not** apply clips to drawing output. The `SkClipStack` is maintained for state queries (e.g., `devClipBounds()`, `isClipEmpty()`) and for `SkCanvas`-level quick-reject optimization, but sublayers are emitted unclipped. Clip-to-mask translation (converting clip state into `CAShapeLayer` masks) is deferred to a future phase (see Section 9).
2. **Precedent.** All document-oriented (non-pixel) backends in Skia use this base class. The CALayer backend is conceptually similar — it produces a structured output (layer tree) rather than pixels.
3. **Forward compatibility.** When we later implement clip-aware rendering (e.g., applying a `CAShapeLayer` mask for clip paths), the clip stack is already available via `this->cs()`.

**Why not `SkNoDrawCanvas`?** While `SkNoDrawCanvas` provides all draw hooks as no-ops, it operates at the SkCanvas level. Skia's canvas→device delegation path applies transforms, handles image filters, and resolves save layers *before* calling the device. By implementing at the device level, we receive already-transformed geometry with filters pre-applied, dramatically simplifying our code. `SkNoDrawCanvas` is designed for analysis tasks (e.g., bounding-box computation), not rendering backends.

#### 4.2.2 SkCALayerDevice — Header Definition

**File:** `src/calayer/SkCALayerDevice.h`

```cpp
#ifndef SkCALayerDevice_DEFINED
#define SkCALayerDevice_DEFINED

#include "src/core/SkClipStackDevice.h"

// Pure C++ header — no Objective-C types or conditional compilation.
// CALayer interaction goes through the pure C bridge (SkCALayerBridge.h),
// which is implemented in SkCALayerBridge.m (pure Objective-C).
// This header is included only from .cpp files.

class SkCALayerDevice final : public SkClipStackDevice {
public:
    // Factory method — returns nullptr if the platform is unsupported.
    // rootLayer: caller-owned CALayer* passed as void*.
    //   Objective-C callers: pass (__bridge void*)caLayer.
    // The device retains rootLayer for safety (CFRetain); the caller remains the
    // primary owner and can use the layer after the device is destroyed.
    //
    // Internally constructs SkImageInfo via SkImageInfo::MakeUnknown(width, height)
    // (kUnknown_SkColorType, kUnknown_SkAlphaType — matching the SkSVGDevice pattern
    // for non-pixel backends) and default SkSurfaceProps to initialize the
    // SkClipStackDevice base class.
    static sk_sp<SkDevice> Make(const SkISize& size, void* rootLayer);

    // ---- SkDevice drawing overrides ----
    void drawPaint(const SkPaint& paint) override;
    void drawPoints(SkCanvas::PointMode, SkSpan<const SkPoint>, const SkPaint&) override;
    void drawRect(const SkRect& r, const SkPaint& paint) override;   // ← Phase 2 focus
    void drawOval(const SkRect& oval, const SkPaint& paint) override;
    void drawRRect(const SkRRect& rr, const SkPaint& paint) override;
    void drawPath(const SkPath& path, const SkPaint& paint) override;
    void drawImageRect(const SkImage*, const SkRect* src, const SkRect& dst,
                       const SkSamplingOptions&, const SkPaint&,
                       SkCanvas::SrcRectConstraint) override;
    void drawVertices(const SkVertices*, sk_sp<SkBlender>, const SkPaint&, bool) override;
    void drawMesh(const SkMesh&, sk_sp<SkBlender>, const SkPaint&) override;

private:
    SkCALayerDevice(const SkImageInfo& info, const SkSurfaceProps& props, void* rootLayer);
    ~SkCALayerDevice() override;

    void onDrawGlyphRunList(SkCanvas*, const sktext::GlyphRunList&,
                            const SkPaint& paint) override;

    // applyPaintToShapeLayer() is a file-static helper in SkCALayerDevice.cpp.
    // It takes void* (opaque layer handle) and calls C bridge functions.
    // Not exposed in this header.

    // The root layer to which sublayers are added during drawing.
    // Stored as void* (opaque pointer). The constructor calls SkCALayer_Retain(),
    // the destructor calls SkCALayer_Release(). The C bridge implementation
    // (SkCALayerBridge.m) casts to CALayer* internally.
    void* fRootLayer;
};

#endif // SkCALayerDevice_DEFINED
```

**Non-draw SkDevice virtuals.** Beyond the drawing hooks listed above, `SkDevice` declares virtual functions for clip management, surface creation, pixel read-back, save-layer compositing, and image-filter support. Their treatment in this phase:

| Category | Functions | Handling |
|---|---|---|
| Clip management | `pushClipStack`, `popClipStack`, `clipRect`, `clipRRect`, `clipPath`, `clipRegion`, `replaceClip`, `isClip*`, `devClipBounds`, `onClipShader` | **State tracking only** — implemented by `SkClipStackDevice` base class. Clip state is maintained for queries and `SkCanvas`-level quick-reject, but **not applied to drawing output**: sublayers are emitted unclipped. Clip-to-mask translation is deferred (see Section 9) |
| Surface creation | `makeSurface(const SkImageInfo&, const SkSurfaceProps&)` | Default returns `nullptr` — acceptable; the backend produces a layer tree, not pixel surfaces |
| Save-layer device | `createDevice(const CreateInfo&, const SkPaint*)` | Default returns `nullptr` — **content loss**: `SkCanvas` falls back to `SkNoPixelsDevice`, silently discarding all draw calls within the `saveLayer()` scope. This also affects draw calls whose paint carries image filters or mask filters, since `SkCanvas` internally uses `saveLayer()` to process them (see Image-filter support below). `saveLayer()` compositing is not supported in Phase 2 (see Future Work) |
| Image-filter support | `snapSpecial`, `makeSpecial`, `drawSpecial`, `drawCoverageMask` | Defaults (`nullptr` / no-op) — **content loss**: when `SkCanvas` encounters a paint with an image filter, it creates a save layer via `createDevice()`, which returns `nullptr` and falls back to `SkNoPixelsDevice`. The draw call is routed to `SkNoPixelsDevice` (a no-op) and never reaches `SkCALayerDevice` — the content is silently lost. Callers must not use image filters on paints in Phase 2 |
| Layer compositing | `drawDevice(SkDevice*, ...)` | Default — not needed until `createDevice` is implemented |
| Pixel read-back | `onReadPixels`, `onWritePixels`, `onPeekPixels`, `onAccessPixels` | Default `false` — the backend produces layers, not pixels |

These defaults are acceptable because the CALayer backend is a non-pixel, document-oriented backend (like SVG/PDF). Functions that require pixel access or save-layer compositing will be overridden in future phases as needed.

#### 4.2.3 Pure C Bridge API (`SkCALayerBridge.h` / `SkCALayerBridge.m`)

The C++ device implementation and the Objective-C Core Animation code never coexist in the same translation unit. Instead, they communicate through a **pure C header** (`SkCALayerBridge.h`) with opaque `void*` handles and `extern "C"` linkage. The bridge header is included from both `.cpp` (C++) and `.m` (Objective-C) files — it contains no C++ classes, no `@class` declarations, and no ARC annotations.

**File:** `src/calayer/SkCALayerBridge.h`

```c
/* SkCALayerBridge.h — Pure C interface to Core Animation.
 * Included from both .cpp (C++) and .m (Objective-C).
 * No C++ classes. No @class. No ARC annotations. */

#ifndef SkCALayerBridge_DEFINED
#define SkCALayerBridge_DEFINED

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Layer lifecycle --- */
void* SkCALayer_CreateShapeLayer(void);          /* +1 retained CAShapeLayer */
void  SkCALayer_AddSublayer(void* parent, void* child);
void  SkCALayer_Retain(void* layer);             /* CFRetain */
void  SkCALayer_Release(void* layer);            /* CFRelease */

/* --- CAShapeLayer properties --- */
void SkCAShapeLayer_SetPath(void* layer, CGPathRef path);
void SkCAShapeLayer_SetAnchorPoint(void* layer, CGFloat x, CGFloat y);
void SkCAShapeLayer_SetPosition(void* layer, CGFloat x, CGFloat y);
void SkCAShapeLayer_SetAffineTransform(void* layer, CGAffineTransform t);
void SkCAShapeLayer_SetFillColor(void* layer, CGColorRef color);    /* NULL to clear */
void SkCAShapeLayer_SetStrokeColor(void* layer, CGColorRef color);  /* NULL to clear */
void SkCAShapeLayer_SetLineWidth(void* layer, CGFloat width);
void SkCAShapeLayer_SetLineCap(void* layer, int cap);               /* 0=Butt 1=Round 2=Square */
void SkCAShapeLayer_SetLineJoin(void* layer, int join);             /* 0=Miter 1=Round 2=Bevel */
void SkCAShapeLayer_SetMiterLimit(void* layer, CGFloat limit);

/* --- Queries (for tests) --- */
int       SkCALayer_GetSublayerCount(void* layer);
void*     SkCALayer_GetSublayerAtIndex(void* layer, int index);
CGPathRef SkCAShapeLayer_CopyPath(void* layer);                     /* +1 retained */
CGColorRef SkCAShapeLayer_GetFillColor(void* layer);                /* not retained */
CGColorRef SkCAShapeLayer_GetStrokeColor(void* layer);              /* not retained */
CGFloat   SkCAShapeLayer_GetLineWidth(void* layer);
CGFloat   SkCAShapeLayer_GetMiterLimit(void* layer);
CGAffineTransform SkCAShapeLayer_GetAffineTransform(void* layer);
CGFloat   SkCALayer_GetOpacity(void* layer);

/* --- Transaction (implicit-animation control) --- */
void SkCATransaction_BeginDisablingActions(void);  /* begin + setDisableActions:YES */
void SkCATransaction_Commit(void);

/* --- Debug --- */
bool SkCALayer_IsMainThread(void);

#ifdef __cplusplus
}
#endif

#endif /* SkCALayerBridge_DEFINED */
```

**Memory ownership convention.** Functions named `Create` or `Copy` return a +1 retained reference — the caller must call `SkCALayer_Release()` when done. `SkCALayer_AddSublayer()` causes the parent layer to retain the child (per Core Animation semantics), so the caller should release its own reference after adding. Query functions (e.g., `SkCAShapeLayer_GetFillColor`) return unretained references.

**File:** `src/calayer/SkCALayerBridge.m` (compiled as **pure Objective-C** with `-fobjc-arc`)

```objc
/* SkCALayerBridge.m — pure Objective-C, no C++ */
#import <QuartzCore/QuartzCore.h>
#import "SkCALayerBridge.h"

/* --- Lifecycle --- */
void* SkCALayer_CreateShapeLayer(void) {
    CAShapeLayer* layer = [CAShapeLayer layer];
    return (__bridge_retained void*)layer;       /* +1 to caller */
}

void SkCALayer_AddSublayer(void* parent, void* child) {
    [(__bridge CALayer*)parent addSublayer:(__bridge CALayer*)child];
}

void SkCALayer_Retain(void* layer)  { CFRetain(layer); }
void SkCALayer_Release(void* layer) { CFRelease(layer); }

/* --- Property setters --- */
void SkCAShapeLayer_SetPath(void* layer, CGPathRef path) {
    ((__bridge CAShapeLayer*)layer).path = path;
}

void SkCAShapeLayer_SetAnchorPoint(void* layer, CGFloat x, CGFloat y) {
    ((__bridge CALayer*)layer).anchorPoint = CGPointMake(x, y);
}

void SkCAShapeLayer_SetPosition(void* layer, CGFloat x, CGFloat y) {
    ((__bridge CALayer*)layer).position = CGPointMake(x, y);
}

void SkCAShapeLayer_SetAffineTransform(void* layer, CGAffineTransform t) {
    ((__bridge CALayer*)layer).affineTransform = t;
}

void SkCAShapeLayer_SetFillColor(void* layer, CGColorRef color) {
    ((__bridge CAShapeLayer*)layer).fillColor = color;
}

void SkCAShapeLayer_SetStrokeColor(void* layer, CGColorRef color) {
    ((__bridge CAShapeLayer*)layer).strokeColor = color;
}

void SkCAShapeLayer_SetLineWidth(void* layer, CGFloat width) {
    ((__bridge CAShapeLayer*)layer).lineWidth = width;
}

static NSString* const kCapMap[] = {
    kCALineCapButt, kCALineCapRound, kCALineCapSquare
};
void SkCAShapeLayer_SetLineCap(void* layer, int cap) {
    ((__bridge CAShapeLayer*)layer).lineCap = kCapMap[cap];
}

static NSString* const kJoinMap[] = {
    kCALineJoinMiter, kCALineJoinRound, kCALineJoinBevel
};
void SkCAShapeLayer_SetLineJoin(void* layer, int join) {
    ((__bridge CAShapeLayer*)layer).lineJoin = kJoinMap[join];
}

void SkCAShapeLayer_SetMiterLimit(void* layer, CGFloat limit) {
    ((__bridge CAShapeLayer*)layer).miterLimit = limit;
}

/* --- Queries --- */
int SkCALayer_GetSublayerCount(void* layer) {
    return (int)((__bridge CALayer*)layer).sublayers.count;
}

void* SkCALayer_GetSublayerAtIndex(void* layer, int index) {
    return (__bridge void*)((__bridge CALayer*)layer).sublayers[index];
}

CGPathRef SkCAShapeLayer_CopyPath(void* layer) {
    return CGPathRetain(((__bridge CAShapeLayer*)layer).path);
}

CGColorRef SkCAShapeLayer_GetFillColor(void* layer) {
    return ((__bridge CAShapeLayer*)layer).fillColor;
}

CGColorRef SkCAShapeLayer_GetStrokeColor(void* layer) {
    return ((__bridge CAShapeLayer*)layer).strokeColor;
}

CGFloat SkCAShapeLayer_GetLineWidth(void* layer) {
    return ((__bridge CAShapeLayer*)layer).lineWidth;
}

CGFloat SkCAShapeLayer_GetMiterLimit(void* layer) {
    return ((__bridge CAShapeLayer*)layer).miterLimit;
}

CGAffineTransform SkCAShapeLayer_GetAffineTransform(void* layer) {
    return ((__bridge CALayer*)layer).affineTransform;
}

CGFloat SkCALayer_GetOpacity(void* layer) {
    return ((__bridge CALayer*)layer).opacity;
}

bool SkCALayer_IsMainThread(void) {
    return [NSThread isMainThread];
}

/* --- Transaction --- */
void SkCATransaction_BeginDisablingActions(void) {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
}

void SkCATransaction_Commit(void) {
    [CATransaction commit];
}
```

Note: `__bridge_retained` in `CreateShapeLayer` transfers ownership from ARC to the caller — ARC will not release the object, and the caller must call `SkCALayer_Release()` (which calls `CFRelease`). The `__bridge` casts in all other functions are zero-cost casts that do not transfer ownership — ARC continues to manage the object's lifetime within the function scope, and the root layer's `sublayers` array retains child layers as needed.

#### 4.2.4 drawRect Implementation Strategy

`drawRect` is the simplest draw hook to implement because it maps directly to a `CAShapeLayer` with a rectangular `CGPath`.

**Call flow (what SkCanvas does before calling our device):**

```
SkCanvas::drawRect(r, paint)
  → r.makeSorted()                         // normalize
  → internalQuickReject(r, paint)          // early-out if clipped away
  → aboutToDraw(paint, &r) or
    attemptBlurredRRectDraw(...)           // image/mask filter handling (see warning below)
  → topDevice()->drawRect(r, layer.paint()) // ← we receive this call (if no saveLayer was created)
```

**Warning — image filters and mask filters cause content loss in Phase 2.** If the original paint carries an image filter, `SkCanvas::aboutToDraw()` (via `AutoLayerForImageFilter`) calls `internalSaveLayer()`, which calls `createDevice()` on our device. Since our `createDevice()` returns `nullptr`, Skia creates an `SkNoPixelsDevice` as the save-layer target. The draw call is then dispatched to `SkNoPixelsDevice::drawRect()` (a no-op), **not** to `SkCALayerDevice::drawRect()`. The content is **silently lost** — our device never sees the draw call. The same applies to mask filters that trigger a save layer internally. Callers must ensure that paints passed to the canvas do not carry image filters or mask filters in Phase 2.

By the time `SkCALayerDevice::drawRect()` is called (for filter-free paints):
- The rect is sorted (non-inverted).
- The current transform is available via two accessors: `this->localToDevice()` returns `const SkMatrix&` (the 3×3 affine matrix, sufficient for 2D transforms), while `this->localToDevice44()` returns `const SkM44&` (the full 4×4 matrix, needed only if 3D/perspective information is required). Since this backend only needs the 2D affine portion, `localToDevice()` is used directly. If the matrix contains a perspective component (`hasPerspective()` returns true), it cannot be represented as a `CGAffineTransform`; the draw call is skipped with a debug warning. Rather than pre-transforming geometry with `mapRect()` — which collapses rotation/skew into an axis-aligned bounding box — the implementation keeps the path in local coordinates and applies the affine portion as the layer's `affineTransform`.

**Implementation (`src/calayer/SkCALayerDevice.cpp`) — pure C++ calling the C bridge:**

```cpp
// SkCALayerDevice.cpp — pure C++, no Objective-C syntax
#include "src/calayer/SkCALayerDevice.h"
#include "src/calayer/SkCALayerBridge.h"
#include "src/calayer/SkCALayerConvert.h"

void SkCALayerDevice::drawRect(const SkRect& r, const SkPaint& paint) {
    // 1. Get the current 3×3 transform matrix.
    //    localToDevice() returns const SkMatrix& (3×3 affine).
    //    CGAffineTransform cannot represent perspective — skip the draw if present.
    const SkMatrix& ctm = this->localToDevice();
    if (ctm.hasPerspective()) {
        SkDEBUGF("SkCALayerDevice::drawRect: perspective transform not supported, "
                 "skipping draw\n");
        return;
    }

    // 2. Disable implicit animations — Core Animation applies a default 0.25 s
    //    animation to every property change. The rendering backend requires
    //    immediate, non-animated updates.
    SkCATransaction_BeginDisablingActions();

    // 3. Create CAShapeLayer via C bridge (+1 retained)
    void* shapeLayer = SkCALayer_CreateShapeLayer();

    // 4. Set path in LOCAL coordinates
    CGRect cgRect = CGRectMake(r.fLeft, r.fTop, r.width(), r.height());
    CGPathRef path = CGPathCreateWithRect(cgRect, NULL);
    SkCAShapeLayer_SetPath(shapeLayer, path);
    CGPathRelease(path);

    // 5. Configure anchorPoint/position so the layer's coordinate origin aligns
    //    with the root layer's origin (see "CALayer coordinate setup" below).
    SkCAShapeLayer_SetAnchorPoint(shapeLayer, 0, 0);  // default (0.5, 0.5) would offset
    SkCAShapeLayer_SetPosition(shapeLayer, 0, 0);

    // 6. Apply the current local-to-device transform as a CALayer affine transform.
    //    Using affineTransform (not mapRect) preserves rotation and skew correctly.
    SkCAShapeLayer_SetAffineTransform(shapeLayer, CGAffineTransformMake(
        ctm.getScaleX(), ctm.getSkewY(),   // a, b
        ctm.getSkewX(),  ctm.getScaleY(),  // c, d
        ctm.getTranslateX(), ctm.getTranslateY()));  // tx, ty

    // 7. Apply paint properties (see applyPaintToShapeLayer below)
    applyPaintToShapeLayer(shapeLayer, paint);

    // 8. Add to root layer, then release our ref (+1 from Create is balanced)
    SkCALayer_AddSublayer(fRootLayer, shapeLayer);
    SkCALayer_Release(shapeLayer);

    // 9. Commit transaction — property changes take effect immediately, no animations.
    SkCATransaction_Commit();
}
```

**CALayer coordinate setup.** Each `CAShapeLayer` produced by `drawRect` requires explicit `anchorPoint` and `position` configuration. `CALayer` defaults to `anchorPoint = (0.5, 0.5)` and `position = (0, 0)`, meaning the layer's center (not its origin) is placed at the superlayer's origin — this would silently offset every shape by half its bounds. Setting `anchorPoint = (0, 0)` and `position = (0, 0)` ensures the layer's coordinate origin coincides with the root layer's origin, so path coordinates in the layer's local space map directly to the superlayer's coordinate space. The `affineTransform` is then applied around this `(0, 0)` anchor point, matching Skia's transform-around-origin semantics. Note that `bounds` is not explicitly set because `CAShapeLayer` derives its visual extent from its `path` property, not from `bounds`.

**iOS vs. macOS coordinate systems.** Skia uses a Y-down coordinate system (origin at top-left). On iOS, `CALayer` also defaults to Y-down (origin at top-left of the superlayer), so no coordinate transform is needed — path coordinates from Skia map directly. On macOS, `CALayer` defaults to Y-up (origin at bottom-left); a future macOS port would need to set `rootLayer.geometryFlipped = YES` or apply a Y-flip transform to the root layer. Phase 2 targets iOS only.

**Paint-to-CAShapeLayer mapping (Phase 2 subset):**

| SkPaint property | CAShapeLayer property | Notes |
|---|---|---|
| `getColor4f()` | `fillColor` / `strokeColor` | RGBA → `CGColor` created in the **sRGB color space** via `CGColorCreate(sRGBSpace, components)`; the alpha channel in the `CGColor` is authoritative — do **not** also set `opacity`, as that would double-apply alpha |
| `getStyle()` (Fill) | `fillColor` set, `strokeColor` = nil | Default |
| `getStyle()` (Stroke) | `strokeColor` set, `fillColor` = nil | |
| `getStyle()` (StrokeAndFill) | Both set | |
| `getStrokeWidth()` | `lineWidth` | Direct mapping |
| `getStrokeCap()` | `lineCap` | `kButt` → `kCALineCapButt`, `kRound` → `kCALineCapRound`, `kSquare` → `kCALineCapSquare` |
| `getStrokeJoin()` | `lineJoin` | `kMiter` → `kCALineJoinMiter`, `kRound` → `kCALineJoinRound`, `kBevel` → `kCALineJoinBevel` |
| `getStrokeMiter()` | `miterLimit` | Direct mapping |
| `isAntiAlias()` | (ignored — CA always anti-aliases) | |

Note on alpha: `SkPaint::getColor()` already encodes the alpha channel. The `CGColor` created from it carries this alpha. `CAShapeLayer.opacity` defaults to `1.0` and should **not** be set from `getAlpha()` — doing so would apply alpha twice (once in the color, once in the layer opacity).

Properties not mapped in this phase (shader, blend mode, mask filter, path effect) are silently ignored.

**`applyPaintToShapeLayer` — file-static helper in `SkCALayerDevice.cpp` (pure C++):**

```cpp
// In SkCALayerDevice.cpp — pure C++, calls C bridge functions
static void applyPaintToShapeLayer(void* shapeLayer, const SkPaint& paint) {
    // Convert SkColor4f to CGColor in the sRGB color space.
    // SkCALayerCreateCGColor is a pure C++ helper from SkCALayerConvert.h.
    SkColor4f c = paint.getColor4f();
    CGColorRef cgColor = SkCALayerCreateCGColor(c);

    // Map SkPaint::Style to fillColor / strokeColor via C bridge.
    // The three modes are mutually exclusive with respect to which color
    // properties are set vs. cleared.
    switch (paint.getStyle()) {
        case SkPaint::kFill_Style:
            SkCAShapeLayer_SetFillColor(shapeLayer, cgColor);
            SkCAShapeLayer_SetStrokeColor(shapeLayer, NULL);   // clear stroke
            break;
        case SkPaint::kStroke_Style:
            SkCAShapeLayer_SetFillColor(shapeLayer, NULL);     // clear fill
            SkCAShapeLayer_SetStrokeColor(shapeLayer, cgColor);
            break;
        case SkPaint::kStrokeAndFill_Style:
            SkCAShapeLayer_SetFillColor(shapeLayer, cgColor);
            SkCAShapeLayer_SetStrokeColor(shapeLayer, cgColor);
            break;
    }
    CGColorRelease(cgColor);

    // Stroke geometry properties via C bridge
    SkCAShapeLayer_SetLineWidth(shapeLayer, paint.getStrokeWidth());
    SkCAShapeLayer_SetMiterLimit(shapeLayer, paint.getStrokeMiter());

    // Cap: kButt=0, kRound=1, kSquare=2 — matches SkPaint enum order
    SkCAShapeLayer_SetLineCap(shapeLayer, static_cast<int>(paint.getStrokeCap()));
    // Join: kMiter=0, kRound=1, kBevel=2 — matches SkPaint enum order
    SkCAShapeLayer_SetLineJoin(shapeLayer, static_cast<int>(paint.getStrokeJoin()));
}
```

Note: `CGColorRef` is a Core Foundation type — it is managed through `CGColorCreate`/`CGColorRelease` (C functions), not by ARC. The `SkCALayerCreateCGColor()` helper in `SkCALayerConvert.h/.cpp` is pure C++ code that calls the CoreGraphics C API directly.

Note on `CGColorSpaceCreateWithName(kCGColorSpaceSRGB)` (called inside `SkCALayerCreateCGColor`): on modern iOS/macOS, the system internally caches named color spaces, so repeated calls return a cached instance rather than allocating new objects. The per-call overhead is negligible for Phase 2's target workload (tens to hundreds of layers). Caching the `CGColorSpaceRef` is a possible micro-optimization but is deferred as unnecessary in this phase.

#### 4.2.5 No-Op Stubs for Unimplemented Methods

All other draw-method overrides are implemented as empty bodies in `SkCALayerDevice.cpp`:

```cpp
void SkCALayerDevice::drawPaint(const SkPaint&) {}
void SkCALayerDevice::drawPoints(SkCanvas::PointMode, SkSpan<const SkPoint>,
                                  const SkPaint&) {}
void SkCALayerDevice::drawOval(const SkRect&, const SkPaint&) {}
void SkCALayerDevice::drawRRect(const SkRRect&, const SkPaint&) {}
void SkCALayerDevice::drawPath(const SkPath&, const SkPaint&) {}
void SkCALayerDevice::drawImageRect(const SkImage*, const SkRect*, const SkRect&,
                                     const SkSamplingOptions&, const SkPaint&,
                                     SkCanvas::SrcRectConstraint) {}
void SkCALayerDevice::drawVertices(const SkVertices*, sk_sp<SkBlender>,
                                    const SkPaint&, bool) {}
void SkCALayerDevice::drawMesh(const SkMesh&, sk_sp<SkBlender>, const SkPaint&) {}
void SkCALayerDevice::onDrawGlyphRunList(SkCanvas*, const sktext::GlyphRunList&,
                                          const SkPaint&) {}
```

Non-draw virtual functions (`makeSurface`, `createDevice`, `snapSpecial`, `onReadPixels`, etc.) are not overridden — their default implementations in `SkDevice` return `nullptr` or `false`, which is correct for a non-pixel backend in Phase 2 (see the virtual function table in Section 4.2.2).

This ensures the device compiles and links with `SkCanvas` immediately, with no undefined-symbol errors.

#### 4.2.6 File Layout

```
skia/
  include/
    calayer/
      SkCALayerCanvas.h          ← public factory API (pure C++)
  src/
    calayer/
      SkCALayerDevice.h          ← internal header (pure C++, uses void* for CALayer)
      SkCALayerDevice.cpp        ← device implementation (pure C++, calls C bridge)
      SkCALayerBridge.h          ← pure C header: extern "C" opaque API for CALayer operations
      SkCALayerBridge.m          ← pure Objective-C implementation (compiled with -fobjc-arc)
      SkCALayerConvert.h         ← pure C++ header: CGColor creation, matrix validation
      SkCALayerConvert.cpp       ← pure C++ implementation (links CoreGraphics)
      BUILD.gn                   ← build target (GN)
  tests/
    CALayerDeviceTest.cpp        ← unit tests (pure C++, inspects layers via C bridge)
  gm/
    calayer_rect.cpp             ← GM visual tests (pure C++, renders via C bridge)
```

There are **no `.mm` (Objective-C++) files** in this design. The language boundary is enforced by file extension:

- `.cpp` files are compiled as **pure C++**. They include `SkCALayerBridge.h` (a C header with `extern "C"` linkage) and `SkCALayerConvert.h` (a C++ header using CoreGraphics C APIs). They never see `@class`, `@selector`, or any Objective-C syntax.
- `.m` files are compiled as **pure Objective-C** (with `-fobjc-arc`). They include `SkCALayerBridge.h` and `<QuartzCore/QuartzCore.h>`. They never see C++ classes, templates, or namespaces.
- `.h` files are either pure C (`SkCALayerBridge.h` — uses `extern "C"`, `void*`, and C types only) or pure C++ (`SkCALayerDevice.h`, `SkCALayerConvert.h`).

`SkCALayerConvert.h/.cpp` contains pure C++ helpers that depend only on CoreGraphics C APIs (not Objective-C). This includes `CGColorCreate`-based color conversion and `SkMatrix` perspective validation. `SkCALayerBridge.m` is the **only** file that uses Objective-C message syntax (`[CAShapeLayer layer]`, `layer.path = ...`) and ARC memory management.

### 4.3 Design Rationale

**SkClipStackDevice vs. SkDevice (direct).** `SkClipStackDevice` saves roughly 200 lines of clip management code and aligns with the pattern used by every non-pixel Skia backend. The minor cost is inheriting a clip stack that the CALayer backend does not yet use for rendering — but having it available will simplify future clip integration.

**SkDevice vs. SkCanvas inheritance.** Considered inheriting from `SkCanvasVirtualEnforcer<SkCanvas>` or `SkNoDrawCanvas` as the user suggested. Analysis of the codebase shows that SkCanvas performs significant pre-processing before delegating to the device:

- Transform application via `localToDevice()`
- Image filter handling via `aboutToDraw()` / save-layer machinery
- Quick-reject optimization via `internalQuickReject()`
- Blur mask filter fast-path via `attemptBlurredRRectDraw()`

Reimplementing any of this at the canvas level would be error-prone and duplicate existing code. The SkDevice approach gets all of this for free.

**Pure C bridge (no Objective-C++).** The design strictly separates C++ and Objective-C into different translation units. There are no `.mm` (Objective-C++) files — C++ code lives in `.cpp`, Objective-C code lives in `.m`, and they communicate exclusively through a pure C header (`SkCALayerBridge.h`) with `extern "C"` linkage and `void*` opaque handles. This eliminates mixed-language translation units entirely, giving each compiler (clang C++ vs. clang ObjC) its own clean scope. The `extern "C"` + `void*` pattern follows the precedent set by Skia's `GrMTLHandle` (`typedef const void*`) in `gpu/mtl/GrMtlTypes.h` for Metal interop, and by Apple's own CoreFoundation framework (e.g., `CFStringRef`, `CFArrayRef` — all opaque `const void*` pointers behind C functions).

**Disabling implicit animations.** Core Animation applies a default 0.25-second implicit animation whenever an "animatable" `CALayer` property is modified (e.g., `path`, `fillColor`, `strokeColor`, `position`, `transform`, `opacity`). For a rendering backend that constructs layer trees as immediate-mode drawing output, these animations are undesirable — they would cause newly created sublayers to visually "fade in" or "slide" to their target state instead of appearing instantly. The implementation wraps all layer-property mutations inside a `CATransaction` with `setDisableActions:YES`, ensuring every property change takes effect on the next commit (i.e., the next display refresh) with no animation. The `CATransaction` begin/commit pair is scoped per draw call (e.g., one pair per `drawRect` invocation) via the C bridge functions `SkCATransaction_BeginDisablingActions()` / `SkCATransaction_Commit()`. This approach is lightweight (`CATransaction` is a per-thread stack), well-documented by Apple, and does not affect the caller's ability to subsequently animate layer properties after the canvas is destroyed.

**`CAShapeLayer` for drawRect (not `CALayer.bounds`).** A plain `CALayer` with `bounds`/`backgroundColor` could render a filled rect, but cannot handle stroked rects. `CAShapeLayer` with a `CGPath` handles both fill and stroke uniformly, and extends naturally to `drawRRect`, `drawOval`, and `drawPath` in the future.

**ARC isolation.** ARC (`-fobjc-arc`) is applied only to the `.m` bridge implementation file (`SkCALayerBridge.m`). The `.m` file uses `__bridge_retained` (for `Create` functions, transferring +1 ownership to the C caller) and `__bridge` (for property access, zero-cost cast within ARC scope). The C++ side never touches ARC — it uses `SkCALayer_Retain()`/`SkCALayer_Release()` (which call `CFRetain`/`CFRelease`) for explicit reference counting. This is simpler and safer than the ObjC++ approach, where ARC and manual `CFRetain`/`CFRelease` coexist in the same translation unit and the developer must track which variables ARC manages and which it does not.

**Memory ownership convention.** Bridge functions named `Create` or `Copy` return +1 retained references — the caller must balance with `SkCALayer_Release()`. `SkCALayer_AddSublayer()` causes the parent to retain the child (per Core Animation semantics), so the caller should release its own reference after adding. This follows the Core Foundation "Create Rule" and is identical to how `CGPathCreate*`/`CGPathRelease` and `CGColorCreate`/`CGColorRelease` work.

**Main-thread requirement.** All `SkCALayerDevice` drawing operations ultimately call bridge functions that manipulate `CALayer` objects. Core Animation requires layer-tree mutations to occur on the main thread. Callers must ensure that canvas creation, drawing, and the final layer-tree handoff all happen on the main thread. This constraint is documented on the public factory (`SkCALayerCanvas::Make`) and enforced by an `assert(SkCALayer_IsMainThread())` in debug builds (the bridge function calls `[NSThread isMainThread]` in the `.m` implementation).

**Frame lifecycle and sublayer accumulation.** The current design adds sublayers to the caller-provided `rootLayer` during drawing but does not remove them. If a caller reuses the same `rootLayer` across multiple frames (create canvas → draw → destroy canvas → repeat), sublayers from previous frames will accumulate. This is by design for Phase 2: the device is a write-only recorder, like `SkSVGDevice` writing to a stream. The caller is responsible for clearing old sublayers before re-drawing, e.g., `[rootLayer.sublayers makeObjectsPerformSelector:@selector(removeFromSuperlayer)]`, or by creating a fresh `CALayer` per frame. A convenience `reset` API may be added in a future phase if usage patterns warrant it.

**Caller-owned root layer.** The public factory `SkCALayerCanvas::Make(size, rootLayer)` takes a caller-owned `CALayer*` rather than creating one internally. This eliminates ownership-transfer ambiguity: the caller creates the layer, passes it in, draws, destroys the canvas, and retains full ownership of the populated layer tree. The device holds a `CFRetain`-ed reference for its own lifetime, but the caller is the primary owner. This mirrors the pattern in `SkSVGCanvas::Make`, which takes a caller-owned `SkWStream*`.

## 5. Interface Changes

**New public API:**

**File:** `include/calayer/SkCALayerCanvas.h`

```cpp
#ifndef SkCALayerCanvas_DEFINED
#define SkCALayerCanvas_DEFINED

#include "include/core/SkCanvas.h"

// Pure C++ header — void* is used for the CALayer parameter.
// No Objective-C types or conditional compilation.

class SK_API SkCALayerCanvas {
public:
    // Creates an SkCanvas that records drawing operations as sublayers of rootLayer.
    //
    // rootLayer — caller-owned CALayer* passed as void*.
    //             Objective-C callers: pass (__bridge void*)caLayer.
    //             Must not be nullptr. The canvas retains rootLayer for its lifetime.
    // Returns nullptr on non-Apple platforms.
    //
    // Threading: must be called on the main thread.
    static std::unique_ptr<SkCanvas> Make(const SkISize& size, void* rootLayer);
};

#endif // SkCALayerCanvas_DEFINED
```

Note: `SkCanvas(sk_sp<SkDevice>)` is annotated "Private. For internal use only." The `SkCALayerCanvas::Make` factory encapsulates device creation internally, following the pattern established by `SkSVGCanvas::Make` for non-pixel backends.

**Usage example (Objective-C caller):**

```objc
#import "include/calayer/SkCALayerCanvas.h"
#import <QuartzCore/QuartzCore.h>

// Caller creates and owns the root layer
CALayer* rootLayer = [CALayer layer];
rootLayer.bounds = CGRectMake(0, 0, 320, 480);
// Note: geometryFlipped is NOT needed on iOS — CALayer defaults to Y-down
// (origin at top-left), matching Skia's coordinate system. On macOS, a future
// port would need rootLayer.geometryFlipped = YES (see Section 4.2.3).

// Create canvas — sublayers will be added to rootLayer
// Pass CALayer* as void* via __bridge cast (ABI-safe, no conditional compilation)
auto canvas = SkCALayerCanvas::Make(SkISize::Make(320, 480), (__bridge void*)rootLayer);

// Draw (paint must NOT carry image filters or mask filters — see Section 4.2.3)
SkPaint paint;
paint.setColor(SK_ColorRED);
paint.setStyle(SkPaint::kFill_Style);
canvas->drawRect(SkRect::MakeXYWH(10, 20, 100, 50), paint);

// Destroy canvas; rootLayer now contains a CAShapeLayer sublayer
canvas.reset();

// Add to UIKit view — caller still owns rootLayer
[myUIView.layer addSublayer:rootLayer];
```

## 6. Testing Strategy

Testing is structured in three tiers, each leveraging a different part of Skia's test infrastructure.

### 6.1 Tier 1: Unit Tests (`tests/CALayerDeviceTest.cpp`)

Unit tests verify the structural output of the device — that drawing operations produce the expected CALayer tree with the correct properties. These tests do **not** compare pixels; they inspect the layer hierarchy through the C bridge query functions.

**Framework:** Skia's `DEF_TEST` / `REPORTER_ASSERT` macros from `tests/Test.h`.

**Approach:** Create a root layer via `SkCALayer_CreateShapeLayer()` (C bridge), pass it to `SkCALayerCanvas::Make()`, issue draw commands via the returned `SkCanvas`, then inspect the layer tree through C bridge query functions (`SkCALayer_GetSublayerCount`, `SkCAShapeLayer_GetFillColor`, etc.). This is analogous to `SVGDeviceTest.cpp`, which creates an `SkSVGDevice`, draws into it, then parses the resulting SVG XML to verify structure. The test file is **pure C++** — no Objective-C syntax — because all layer inspection goes through the C bridge.

**Platform requirement:** Tests must run on macOS/iOS because the C bridge functions internally instantiate real `CALayer` objects. On other platforms, the tests should be conditionally compiled out (guarded by `#ifdef SK_BUILD_FOR_MAC` or `#ifdef SK_BUILD_FOR_IOS`).

### 6.2 Tier 2: GM Tests (`gm/calayer_rect.cpp`)

GM (Golden Master) tests produce visual output that can be compared against reference images. For the CALayer backend, the GM test will:

1. Draw a known pattern using `SkCanvas` on the CALayer device.
2. Render the resulting `CALayer` tree into a `CGContext` bitmap (via a C bridge function that internally calls `[CALayer renderInContext:]`).
3. Copy that bitmap into an `SkBitmap` for comparison.

This allows the DM test runner to compare the CALayer backend's output against the same pattern rendered by the raster backend.

**Framework:** Skia's `DEF_SIMPLE_GM` / `DEF_GM` macros from `gm/gm.h`.

### 6.3 Tier 3: DM Sink Integration (Future)

A full `CALayerSink` in `dm/DMSrcSink.h` would allow every existing GM test to run against the CALayer backend, automatically comparing outputs. This is deferred to a future RFC once more draw operations are implemented.

### 6.4 Key Scenarios

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | Single filled rect | `drawRect({10,20,110,70}, fillPaint(red))` | Root layer has 1 sublayer: `CAShapeLayer` with `fillColor` = red, path = rect(10,20,100,50) in local coords, `affineTransform` = identity |
| 2 | Single stroked rect | `drawRect({0,0,50,50}, strokePaint(blue, width=3))` | `CAShapeLayer` with `strokeColor` = blue, `lineWidth` = 3, `fillColor` = nil |
| 3 | Fill-and-stroke rect | `drawRect({0,0,50,50}, strokeAndFillPaint(green, width=2))` | `CAShapeLayer` with both `fillColor` = green and `strokeColor` = green (SkPaint has a single color; StrokeAndFill applies it to both), `lineWidth` = 2 |
| 4 | Multiple rects | Two `drawRect` calls | Root layer has 2 sublayers, in correct z-order (first drawn = bottom sublayer) |
| 5 | Rect with transform | `canvas.translate(50,50)` then `drawRect({0,0,20,20}, ...)` | `CAShapeLayer` path = rect(0,0,20,20) in local coords, `anchorPoint` = (0,0), `affineTransform` contains translation (50,50) |
| 6 | Rect with alpha | `drawRect({0,0,50,50}, paint(alpha=128))` | `CAShapeLayer` `fillColor` has CGColor alpha ≈ 0.5 in sRGB color space; `opacity` remains 1.0 (default) |
| 7 | Rect with partial clip | `clipRect({0,0,60,60})` then `drawRect({30,30,90,90}, ...)` on the device directly | `CAShapeLayer` is created for the full rect (clip masking is deferred to a future phase). Root layer has 1 sublayer. This tests device behavior — quick-reject is a `SkCanvas`-level optimization and is not the device's responsibility |
| 8 | Empty device | No draw calls | Caller's root `CALayer` has 0 sublayers |
| 9 | Rect with perspective | Apply a perspective transform then `drawRect(...)` | Draw is skipped (debug warning). Root layer has 0 sublayers — `CGAffineTransform` cannot represent perspective |
| 10 | Unimplemented op | `drawOval(...)` on CALayer device | No crash, no sublayer added (no-op) |
| 11 | Non-Apple platform | `SkCALayerCanvas::Make(...)` on Linux | Returns `nullptr` |

## 7. Implementation Plan

### Phase 1: Unit Tests (Red) — 1 day

Write the test file `tests/CALayerDeviceTest.cpp` with all key scenario tests from Section 6.4. The tests will reference `SkCALayerCanvas::Make()` and the C bridge query functions, which do not yet exist — they will fail to compile (or link), establishing the "red" state.

- [ ] Create `tests/CALayerDeviceTest.cpp` with platform guard (`#if defined(SK_BUILD_FOR_MAC) || defined(SK_BUILD_FOR_IOS)`)
- [ ] Implement test `CALayerDevice_Create` — verify `SkCALayerCanvas::Make()` returns non-null, caller's root `CALayer` has 0 sublayers
- [ ] Implement test `CALayerDevice_DrawRect_Fill` — scenario #1
- [ ] Implement test `CALayerDevice_DrawRect_Stroke` — scenario #2
- [ ] Implement test `CALayerDevice_DrawRect_StrokeAndFill` — scenario #3
- [ ] Implement test `CALayerDevice_DrawRect_Multiple` — scenario #4
- [ ] Implement test `CALayerDevice_DrawRect_Transform` — scenario #5
- [ ] Implement test `CALayerDevice_DrawRect_Alpha` — scenario #6
- [ ] Implement test `CALayerDevice_DrawRect_PartialClip` — scenario #7
- [ ] Implement test `CALayerDevice_DrawRect_Perspective` — scenario #9
- [ ] Implement test `CALayerDevice_NoOp_Unimplemented` — scenario #10
- [ ] Add GN `BUILD.gn` entries for test target

**Done when:** Test file compiles on macOS, all tests fail (red) due to missing implementation.

### Phase 2: Core Implementation (Green) — 2 days

Implement the `SkCALayerDevice` class with `drawRect` and all no-op stubs.

- [ ] Create `include/calayer/SkCALayerCanvas.h` — public factory API
- [ ] Create `src/calayer/SkCALayerDevice.h` — internal header (pure C++, `void*` for CALayer)
- [ ] Create `src/calayer/SkCALayerBridge.h` — pure C header: `extern "C"` opaque API for CALayer lifecycle, property setters, query functions
- [ ] Create `src/calayer/SkCALayerBridge.m` — pure Objective-C implementation (compiled with `-fobjc-arc`): thin wrappers around `CALayer`/`CAShapeLayer` property access
- [ ] Create `src/calayer/SkCALayerConvert.h/.cpp` — pure C++ helpers:
  - `SkCALayerCreateCGColor()`: `SkColor4f` → `CGColorRef` in sRGB color space
  - `SkCALayerMatrixHasPerspective()`: validate `SkMatrix` for perspective components
- [ ] Create `src/calayer/SkCALayerDevice.cpp` — pure C++ implementation (calls C bridge, no ObjC syntax):
  - `Make(size, rootLayer)` factory: constructs `SkImageInfo::MakeUnknown(w, h)` + default `SkSurfaceProps`, returns `nullptr` on non-Apple platforms, debug-asserts main thread via `SkCALayer_IsMainThread()`
  - Constructor(`SkImageInfo`, `SkSurfaceProps`, `void* rootLayer`): passes info/props to `SkClipStackDevice`, calls `SkCALayer_Retain()`
  - Destructor: calls `SkCALayer_Release()`
  - `drawRect()`: perspective check, shape layer creation via C bridge, affine transform, paint application, add to root and release
  - `applyPaintToShapeLayer()`: file-static helper, Fill/Stroke/StrokeAndFill dispatch via C bridge setters
  - All other draw methods: empty no-op bodies
- [ ] Create `src/calayer/BUILD.gn` — GN library target with Apple-only compilation, `-fobjc-arc` for `.m` only
- [ ] Verify all Phase 1 unit tests pass (green)

**Done when:** All unit tests from Phase 1 pass on macOS. The device can render filled and stroked rectangles as `CAShapeLayer` sublayers.

### Phase 3: GM Test — 1 day

Write a GM test that renders a known rect pattern and verifies visual output.

- [ ] Create `gm/calayer_rect.cpp` with `DEF_SIMPLE_GM` or class-based GM (pure C++)
- [ ] Add `SkCALayer_RenderInContext()` to C bridge; implement CALayer-to-SkBitmap conversion helper in pure C++ (bridge function calls `[CALayer renderInContext:]` in `.m`; C++ side reads the `CGBitmapContext` pixels into `SkBitmap`)
- [ ] Verify the GM runs in the DM test runner on macOS with `--config 8888` (comparing against raster reference)

**Done when:** GM test runs, produces output, and matches (or nearly matches) the raster backend's rendering of the same rect pattern.

### Phase 4: Integration and Documentation — 0.5 day

- [ ] Add the `calayer` module to the top-level Skia build (behind a feature flag, e.g., `skia_enable_calayer_backend`)
- [ ] Update this RFC status to "Implemented" and check off completed tasks

**Done when:** The CALayer backend builds as part of the Skia tree on macOS/iOS, is off by default, and can be enabled via build flag.

## 8. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| CALayer property model does not cover all SkPaint semantics (e.g., complex blend modes, shader fills) | High | Low (for Phase 2) | Phase 2 only targets solid color fill/stroke. Unsupported paint properties are silently ignored. Future RFCs will address complex paints, potentially by falling back to rasterization for individual layers. |
| Coordinate system mismatch across platforms | Low (iOS) / Med (macOS) | Med | On iOS, Core Animation uses Y-down coordinates (origin at top-left), matching Skia — no transform is needed for Phase 2. On macOS, Core Animation defaults to Y-up (origin at bottom-left); a future macOS port would require `CALayer.geometryFlipped = YES` on the root layer or a Y-flip transform. Phase 2 targets iOS only, so this risk is low. |
| Performance regression with deep layer trees (thousands of sublayers) | Med | Low (this RFC) | Out of scope — this RFC targets UI-style content with tens to hundreds of layers. Performance optimization (layer reuse, batching) is deferred to future work. |
| Memory management across C/ObjC bridge boundary | Low | Med | The pure C bridge design simplifies memory management compared to ObjC++: ARC is confined to the `.m` file only, and the C++ side uses explicit `SkCALayer_Retain()`/`SkCALayer_Release()` (which call `CFRetain`/`CFRelease`). `Create` bridge functions use `__bridge_retained` to transfer +1 ownership to the caller; all other bridge functions use `__bridge` (zero-cost, no ownership change). Since C++ code never holds an ObjC object pointer directly — only opaque `void*` — there is no risk of ARC/manual-retain confusion within a single translation unit. Reviewers should verify that every `SkCALayer_CreateShapeLayer()` call is balanced by either `SkCALayer_Release()` or an `SkCALayer_AddSublayer()` + `SkCALayer_Release()` pair. |

## 9. Future Work

- **drawRRect / drawOval / drawPath** — extend `CAShapeLayer` mapping to cover rounded rects, ovals, and arbitrary paths. These are natural next steps since `CAShapeLayer` accepts any `CGPath`.
- **drawImage / drawImageRect** — map to `CALayer.contents` with a `CGImage`. Enables image display without rasterization.
- **Clip-to-mask translation** — convert the `SkClipStack` into `CAShapeLayer` masks applied to sublayers, enabling correct clipping behavior. Note: `SkCanvas::save()`/`restore()` push and pop clip state on a stack; the mask-layer structure must reflect this, potentially requiring a nested `CALayer` group per save level that carries a clip mask. This interaction between the clip stack's push/pop semantics and the layer-tree structure is the main design challenge.
- **Save-layer compositing** — implement `createDevice()` to return a child `SkCALayerDevice`, enabling `saveLayer()` to composite sublayer groups with opacity, blend modes, or image filters. Required for correct rendering of overlapping translucent content.
- **SkPaint shader → CALayer effects** — translate gradient shaders into `CAGradientLayer`, solid color shaders into layer colors.
- **DM Sink integration** — implement a `CALayerSink` in `DMSrcSink.h` to run the entire GM suite against this backend automatically.
- **Layer tree diffing** — for animated content, diff the previous and next layer trees to minimize `CALayer` mutations per frame.
- **macOS support** — validate and enable the backend on macOS (AppKit), which shares the Core Animation framework.

## 10. References

- Skia SkDevice base class: `src/core/SkDevice.h`
- Skia SkClipStackDevice: `src/core/SkClipStackDevice.h`
- Skia SVG backend (reference implementation): `src/svg/SkSVGDevice.h`, `src/svg/SkSVGDevice.cpp`
- Skia PDF backend (reference implementation): `src/pdf/SkPDFDevice.h`
- Skia SkCanvasVirtualEnforcer: `include/core/SkCanvasVirtualEnforcer.h`
- Skia SkNoDrawCanvas: `include/utils/SkNoDrawCanvas.h`
- Skia DM test runner: `dm/DM.cpp`, `dm/DMSrcSink.h`
- Skia GM test framework: `gm/gm.h`
- Skia unit test framework: `tests/Test.h`
- SVG device tests (structural verification pattern): `tests/SVGDeviceTest.cpp`
- Apple Core Animation Programming Guide: [developer.apple.com/documentation/quartzcore](https://developer.apple.com/documentation/quartzcore)
- CAShapeLayer reference: [developer.apple.com/documentation/quartzcore/cashapelayer](https://developer.apple.com/documentation/quartzcore/cashapelayer)

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.5 | 2026-03-24 | Chason Tang | Explicitly disable Core Animation implicit animations: add `SkCATransaction_BeginDisablingActions()` / `SkCATransaction_Commit()` to the C bridge API, wrap `drawRect` layer-property mutations in a transaction with `setDisableActions:YES`, add design rationale paragraph explaining the necessity and approach |
| 1.4 | 2026-03-24 | Chason Tang | Eliminate all Objective-C++ (`.mm`) files. Introduce pure C bridge API (`SkCALayerBridge.h`/`.m`) as the sole interface between C++ and Objective-C — C++ code in `.cpp` calls `extern "C"` bridge functions, Objective-C code in `.m` implements them. Rewrite `drawRect` and `applyPaintToShapeLayer` as pure C++. Move test files from `.mm` to `.cpp` (using C bridge query functions for layer inspection). Update design rationale, risks, and implementation plan accordingly |
| 1.3 | 2026-03-24 | Chason Tang | Replace `#ifdef __OBJC__` conditional compilation in headers with opaque `void*` pointers to fix C++/ObjC++ ABI mismatch (different mangled symbols in .cpp vs .mm TUs); correct clip management description from "Fully implemented" to "State tracking only" — clips are tracked but not applied to drawing output; correct image filter / mask filter / saveLayer descriptions to accurately state content loss behavior (draw calls routed to SkNoPixelsDevice, never reaching SkCALayerDevice) |
| 1.2 | 2026-03-24 | Chason Tang | Fix localToDevice() return type description (const SkMatrix&, not const SkM44&) and code example; fix createDevice() nullptr behavior (Skia creates SkNoPixelsDevice, draws are silently dropped, not rendered to current device); fix root layer ownership inconsistencies (remove nonexistent rootLayer() getter from diagram, align test section with caller-owned push model) |
| 1.1 | 2026-03-23 | Chason Tang | Address review feedback: fix constructor signature (SkImageInfo+SkSurfaceProps); add perspective matrix validation; specify sRGB color space for CGColor; document anchorPoint/position/bounds setup; clarify iOS Y-down vs macOS Y-up; add applyPaintToShapeLayer pseudocode; separate ObjC/C++ source files; replace test scenario 7 with device-level partial-clip test; add perspective rejection test; document frame lifecycle and sublayer accumulation |
| 1.0 | 2026-03-20 | Chason Tang | Initial version |
