# RFC-001: SkDevice CALayer Rendering Backend for iOS

**Version:** 1.0
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
- **`saveLayer()` compositing** — `createDevice()` returns `nullptr`, so `SkCanvas::saveLayer()` will not create an isolation layer. The save/restore stack still functions (save count increments, clip and transform state is preserved), but subsequent draw calls render directly to the current device without the compositing semantics that `saveLayer()` normally provides (e.g., group opacity, blend modes applied to a flattened sublayer). Callers must avoid relying on `saveLayer()` for correct rendering in Phase 2. This limitation is documented in the virtual function table (Section 4.2.2) and will be addressed in a future RFC (see Section 9, "Save-layer compositing").

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
                          │  CALayer* rootLayer()                 │
                          │    → returns the accumulated tree    │
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

1. **Clip stack management.** `SkClipStackDevice` provides a fully implemented `SkClipStack` with `pushClipStack()`, `popClipStack()`, `clipRect()`, `clipRRect()`, `clipPath()`, `clipRegion()`, `replaceClip()`, and all `isClip*()` queries. We inherit all of this without writing a single line of clip code.
2. **Precedent.** All document-oriented (non-pixel) backends in Skia use this base class. The CALayer backend is conceptually similar — it produces a structured output (layer tree) rather than pixels.
3. **Forward compatibility.** When we later implement clip-aware rendering (e.g., applying a `CAShapeLayer` mask for clip paths), the clip stack is already available via `this->cs()`.

**Why not `SkNoDrawCanvas`?** While `SkNoDrawCanvas` provides all draw hooks as no-ops, it operates at the SkCanvas level. Skia's canvas→device delegation path applies transforms, handles image filters, and resolves save layers *before* calling the device. By implementing at the device level, we receive already-transformed geometry with filters pre-applied, dramatically simplifying our code. `SkNoDrawCanvas` is designed for analysis tasks (e.g., bounding-box computation), not rendering backends.

#### 4.2.2 SkCALayerDevice — Header Definition

**File:** `src/calayer/SkCALayerDevice.h`

```cpp
#ifndef SkCALayerDevice_DEFINED
#define SkCALayerDevice_DEFINED

#include "src/core/SkClipStackDevice.h"

#ifdef __OBJC__
#import <QuartzCore/QuartzCore.h>
#endif

// Forward-declare CALayer for C++ translation units
#ifdef __OBJC__
@class CALayer;
@class CAShapeLayer;
#else
using CALayer = void;
using CAShapeLayer = void;
#endif

class SkCALayerDevice final : public SkClipStackDevice {
public:
    // Factory method — returns nullptr if the platform is unsupported.
    // rootLayer: caller-owned CALayer to which sublayers are added during drawing.
    // The device retains rootLayer for safety (CFRetain); the caller remains the
    // primary owner and can use the layer after the device is destroyed.
    //
    // Internally constructs SkImageInfo via SkImageInfo::MakeUnknown(width, height)
    // (kUnknown_SkColorType, kUnknown_SkAlphaType — matching the SkSVGDevice pattern
    // for non-pixel backends) and default SkSurfaceProps to initialize the
    // SkClipStackDevice base class.
    static sk_sp<SkDevice> Make(const SkISize& size, CALayer* rootLayer);

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
    SkCALayerDevice(const SkImageInfo& info, const SkSurfaceProps& props, CALayer* rootLayer);
    ~SkCALayerDevice() override;

    void onDrawGlyphRunList(SkCanvas*, const sktext::GlyphRunList&,
                            const SkPaint& paint) override;

    // Helpers
    void applyPaintToShapeLayer(CAShapeLayer* layer, const SkPaint& paint);

    // The root layer to which sublayers are added during drawing.
    // Stored as CFTypeRef for C++ header compatibility; the constructor
    // retains via CFRetain, the destructor releases via CFRelease.
    // Caller owns the layer and passes it to Make(); the device holds
    // a retained reference for its lifetime.
    void* fRootLayer;   // CALayer* (bridged)
};

#endif // SkCALayerDevice_DEFINED
```

**Non-draw SkDevice virtuals.** Beyond the drawing hooks listed above, `SkDevice` declares virtual functions for clip management, surface creation, pixel read-back, save-layer compositing, and image-filter support. Their treatment in this phase:

| Category | Functions | Handling |
|---|---|---|
| Clip management | `pushClipStack`, `popClipStack`, `clipRect`, `clipRRect`, `clipPath`, `clipRegion`, `replaceClip`, `isClip*`, `devClipBounds`, `onClipShader` | Fully implemented by `SkClipStackDevice` base class |
| Surface creation | `makeSurface(const SkImageInfo&, const SkSurfaceProps&)` | Default returns `nullptr` — acceptable; the backend produces a layer tree, not pixel surfaces |
| Save-layer device | `createDevice(const CreateInfo&, const SkPaint*)` | Default returns `nullptr` — `saveLayer()` compositing is not supported in Phase 2 (see Future Work) |
| Image-filter support | `snapSpecial`, `makeSpecial`, `drawSpecial`, `drawCoverageMask` | Defaults (`nullptr` / no-op) — image filters on the CALayer device are deferred |
| Layer compositing | `drawDevice(SkDevice*, ...)` | Default — not needed until `createDevice` is implemented |
| Pixel read-back | `onReadPixels`, `onWritePixels`, `onPeekPixels`, `onAccessPixels` | Default `false` — the backend produces layers, not pixels |

These defaults are acceptable because the CALayer backend is a non-pixel, document-oriented backend (like SVG/PDF). Functions that require pixel access or save-layer compositing will be overridden in future phases as needed.

#### 4.2.3 drawRect Implementation Strategy

`drawRect` is the simplest draw hook to implement because it maps directly to a `CAShapeLayer` with a rectangular `CGPath`.

**Call flow (what SkCanvas does before calling our device):**

```
SkCanvas::drawRect(r, paint)
  → r.makeSorted()                         // normalize
  → internalQuickReject(r, paint)          // early-out if clipped away
  → aboutToDraw(paint, &r) or
    attemptBlurredRRectDraw(...)           // handle image/mask filters
  → topDevice()->drawRect(r, layer.paint()) // ← we receive this call
```

By the time `SkCALayerDevice::drawRect()` is called:
- The rect is sorted (non-inverted).
- The paint has had image filters stripped (they were applied as a layer).
- The current transform is available via `this->localToDevice()` (returns `const SkM44&`, a 4×4 matrix). The 3×3 submatrix is extracted via `asM33()`. If the matrix contains a perspective component (third row ≠ `[0, 0, 1]`), it cannot be represented as a `CGAffineTransform`; the draw call is skipped with a debug warning. Rather than pre-transforming geometry with `mapRect()` — which collapses rotation/skew into an axis-aligned bounding box — the implementation keeps the path in local coordinates and applies the affine portion as the layer's `affineTransform`.

**Implementation (`src/calayer/SkCALayerDevice.mm`):**

```objc
void SkCALayerDevice::drawRect(const SkRect& r, const SkPaint& paint) {
    // 1. Extract the 3×3 submatrix from the 4×4 SkM44 returned by localToDevice().
    //    CGAffineTransform cannot represent perspective — skip the draw if present.
    SkMatrix ctm = this->localToDevice().asM33();
    if (ctm.hasPerspective()) {
        SkDEBUGF("SkCALayerDevice::drawRect: perspective transform not supported, "
                 "skipping draw\n");
        return;
    }

    // 2. Create CAShapeLayer with path in LOCAL coordinates
    CAShapeLayer* shapeLayer = [CAShapeLayer layer];
    CGRect cgRect = CGRectMake(r.fLeft, r.fTop, r.width(), r.height());
    CGPathRef path = CGPathCreateWithRect(cgRect, NULL);
    shapeLayer.path = path;
    CGPathRelease(path);  // shapeLayer.path is @property(copy); release our ref

    // 3. Configure anchorPoint/position so the layer's coordinate origin aligns
    //    with the root layer's origin (see "CALayer coordinate setup" below).
    shapeLayer.anchorPoint = CGPointZero;   // default (0.5, 0.5) would offset the shape
    shapeLayer.position = CGPointZero;

    // 4. Apply the current local-to-device transform as a CALayer affine transform.
    //    Using affineTransform (not mapRect) preserves rotation and skew correctly.
    shapeLayer.affineTransform = CGAffineTransformMake(
        ctm.getScaleX(), ctm.getSkewY(),   // a, b
        ctm.getSkewX(),  ctm.getScaleY(),  // c, d
        ctm.getTranslateX(), ctm.getTranslateY());  // tx, ty

    // 5. Apply paint properties (see applyPaintToShapeLayer below)
    this->applyPaintToShapeLayer(shapeLayer, paint);

    // 6. Add to root layer
    [(__bridge CALayer*)fRootLayer addSublayer:shapeLayer];
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

**`applyPaintToShapeLayer` implementation (`SkCALayerDevice.mm`):**

```objc
void SkCALayerDevice::applyPaintToShapeLayer(CAShapeLayer* layer,
                                              const SkPaint& paint) {
    // Convert SkColor4f to CGColor in the sRGB color space.
    // CGColorCreate requires an explicit CGColorSpaceRef — omitting it is a
    // compile error, not a default-to-sRGB convenience.
    SkColor4f c = paint.getColor4f();
    CGFloat components[] = {c.fR, c.fG, c.fB, c.fA};
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGColorRef cgColor = CGColorCreate(srgb, components);
    CGColorSpaceRelease(srgb);

    // Map SkPaint::Style to fillColor / strokeColor.
    // The three modes are mutually exclusive with respect to which color
    // properties are set vs. cleared.
    switch (paint.getStyle()) {
        case SkPaint::kFill_Style:
            layer.fillColor   = cgColor;
            layer.strokeColor = nil;      // explicitly clear stroke
            break;
        case SkPaint::kStroke_Style:
            layer.fillColor   = nil;      // explicitly clear fill
            layer.strokeColor = cgColor;
            break;
        case SkPaint::kStrokeAndFill_Style:
            layer.fillColor   = cgColor;
            layer.strokeColor = cgColor;  // same color for both
            break;
    }
    CGColorRelease(cgColor);

    // Stroke geometry properties
    layer.lineWidth  = paint.getStrokeWidth();
    layer.miterLimit = paint.getStrokeMiter();

    switch (paint.getStrokeCap()) {
        case SkPaint::kButt_Cap:   layer.lineCap  = kCALineCapButt;   break;
        case SkPaint::kRound_Cap:  layer.lineCap  = kCALineCapRound;  break;
        case SkPaint::kSquare_Cap: layer.lineCap  = kCALineCapSquare; break;
    }
    switch (paint.getStrokeJoin()) {
        case SkPaint::kMiter_Join: layer.lineJoin = kCALineJoinMiter; break;
        case SkPaint::kRound_Join: layer.lineJoin = kCALineJoinRound; break;
        case SkPaint::kBevel_Join: layer.lineJoin = kCALineJoinBevel; break;
    }
}
```

Note: `CGColorRef` is a Core Foundation type and is **not** managed by ARC. The explicit `CGColorCreate`/`CGColorRelease` pair is required even when compiling with `-fobjc-arc`.

Note on `CGColorSpaceCreateWithName(kCGColorSpaceSRGB)`: this call appears on every `applyPaintToShapeLayer` invocation. On modern iOS/macOS, the system internally caches named color spaces, so repeated calls return a cached instance rather than allocating new objects. The per-call overhead is negligible for Phase 2's target workload (tens to hundreds of layers). Caching the `CGColorSpaceRef` as a class member or static is a possible micro-optimization but is deferred as unnecessary in this phase.

#### 4.2.4 No-Op Stubs for Unimplemented Methods

All other draw-method overrides are implemented as empty bodies:

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

#### 4.2.5 File Layout

```
skia/
  include/
    calayer/
      SkCALayerCanvas.h          ← public factory API (pure C++ / ObjC compatible)
  src/
    calayer/
      SkCALayerDevice.h          ← internal header (pure C++, uses void* for CALayer)
      SkCALayerDevice.mm         ← ObjC++ device implementation (compiled with -fobjc-arc):
                                    CALayer/CAShapeLayer creation and property setting
      SkCALayerConvert.h         ← pure C++ header: CGColor creation, matrix validation
      SkCALayerConvert.cpp       ← pure C++ implementation (links CoreGraphics, no -fobjc-arc)
      BUILD.gn                   ← build target (GN)
  tests/
    CALayerDeviceTest.mm         ← unit tests (Objective-C++)
  gm/
    calayer_rect.mm              ← GM visual tests (Objective-C++)
```

`SkCALayerConvert.h/.cpp` contains pure C/C++ helpers that depend only on CoreGraphics C APIs (not Objective-C). This includes `CGColorCreate`-based color conversion, `SkMatrix` perspective validation, and stroke property extraction. These files are compiled as standard C++ without `-fobjc-arc`, keeping Objective-C++ compilation surface to a minimum. The `.mm` file imports these helpers and applies the converted values to `CAShapeLayer` using Objective-C message syntax (which requires ObjC++ and ARC).

### 4.3 Design Rationale

**SkClipStackDevice vs. SkDevice (direct).** `SkClipStackDevice` saves roughly 200 lines of clip management code and aligns with the pattern used by every non-pixel Skia backend. The minor cost is inheriting a clip stack that the CALayer backend does not yet use for rendering — but having it available will simplify future clip integration.

**SkDevice vs. SkCanvas inheritance.** Considered inheriting from `SkCanvasVirtualEnforcer<SkCanvas>` or `SkNoDrawCanvas` as the user suggested. Analysis of the codebase shows that SkCanvas performs significant pre-processing before delegating to the device:

- Transform application via `localToDevice()`
- Image filter handling via `aboutToDraw()` / save-layer machinery
- Quick-reject optimization via `internalQuickReject()`
- Blur mask filter fast-path via `attemptBlurredRRectDraw()`

Reimplementing any of this at the canvas level would be error-prone and duplicate existing code. The SkDevice approach gets all of this for free.

**Objective-C++ (.mm) source files.** Core Animation APIs require Objective-C message syntax. Using `.mm` files is standard practice in Skia's Apple-platform code (see `src/ports/`, `tools/skottie_ios_app/`). The header is designed to be includable from both pure C++ (via `void*` bridging) and Objective-C++ contexts.

**`CAShapeLayer` for drawRect (not `CALayer.bounds`).** A plain `CALayer` with `bounds`/`backgroundColor` could render a filled rect, but cannot handle stroked rects. `CAShapeLayer` with a `CGPath` handles both fill and stroke uniformly, and extends naturally to `drawRRect`, `drawOval`, and `drawPath` in the future.

**ARC compilation (`-fobjc-arc`).** All `.mm` implementation files are compiled with Automatic Reference Counting. The header stores the root layer reference as `void*` (a `CFTypeRef`-style opaque pointer) to avoid requiring Objective-C++ compilation for every includer; the `.mm` implementation uses `__bridge` casts to interact with the ARC-managed world. Since ARC does not manage `void*` or `CFTypeRef`, the constructor explicitly calls `CFRetain` and the destructor calls `CFRelease`. Note: the existing Skia Apple-platform code in `src/ports/` and `tools/` uses a mix of MRC and ARC; the `(__bridge void*)` cast pattern is already established in Metal bridge code (e.g., `tools/skottie_ios_app/SkMetalViewBridge.mm`).

**Main-thread requirement.** All `SkCALayerDevice` drawing operations manipulate `CALayer` objects. Core Animation requires layer-tree mutations to occur on the main thread (the thread that runs the `CATransaction` implicit commit at the end of each run-loop cycle). Callers must ensure that canvas creation, drawing, and the final layer-tree handoff all happen on the main thread. This constraint is documented on the public factory (`SkCALayerCanvas::Make`) and enforced by a `NSCAssert([NSThread isMainThread], ...)` in debug builds.

**Objective-C / C++ source separation.** The implementation splits Objective-C++ code (`.mm`, compiled with `-fobjc-arc`) from pure C++ code (`.cpp`). Core Animation layer manipulation (creating `CAShapeLayer`, setting `fillColor`, `lineCap`, etc.) requires Objective-C message syntax and lives in `.mm`. Conversion helpers that only use CoreGraphics C APIs (`CGColorCreate`, `CGPathCreateWithRect`, `CGAffineTransformMake`) live in `.cpp` and compile as standard C++. This separation avoids applying ARC semantics to C++ code, prevents accidental ObjC runtime overhead in pure-C++ paths, and improves build times by limiting the ObjC++ compilation boundary.

**Frame lifecycle and sublayer accumulation.** The current design adds sublayers to the caller-provided `rootLayer` during drawing but does not remove them. If a caller reuses the same `rootLayer` across multiple frames (create canvas → draw → destroy canvas → repeat), sublayers from previous frames will accumulate. This is by design for Phase 2: the device is a write-only recorder, like `SkSVGDevice` writing to a stream. The caller is responsible for clearing old sublayers before re-drawing, e.g., `[rootLayer.sublayers makeObjectsPerformSelector:@selector(removeFromSuperlayer)]`, or by creating a fresh `CALayer` per frame. A convenience `reset` API may be added in a future phase if usage patterns warrant it.

**Caller-owned root layer.** The public factory `SkCALayerCanvas::Make(size, rootLayer)` takes a caller-owned `CALayer*` rather than creating one internally. This eliminates ownership-transfer ambiguity: the caller creates the layer, passes it in, draws, destroys the canvas, and retains full ownership of the populated layer tree. The device holds a `CFRetain`-ed reference for its own lifetime, but the caller is the primary owner. This mirrors the pattern in `SkSVGCanvas::Make`, which takes a caller-owned `SkWStream*`.

## 5. Interface Changes

**New public API:**

**File:** `include/calayer/SkCALayerCanvas.h`

```cpp
#ifndef SkCALayerCanvas_DEFINED
#define SkCALayerCanvas_DEFINED

#include "include/core/SkCanvas.h"

#ifdef __OBJC__
@class CALayer;
#else
using CALayer = void;
#endif

class SK_API SkCALayerCanvas {
public:
    // Creates an SkCanvas that records drawing operations as sublayers of rootLayer.
    //
    // rootLayer — caller-owned CALayer; drawing operations add sublayers to it.
    //             Must not be nullptr. The canvas retains rootLayer for its lifetime.
    // Returns nullptr on non-Apple platforms.
    //
    // Threading: must be called on the main thread.
    static std::unique_ptr<SkCanvas> Make(const SkISize& size, CALayer* rootLayer);
};

#endif // SkCALayerCanvas_DEFINED
```

Note: `SkCanvas(sk_sp<SkDevice>)` is annotated "Private. For internal use only." The `SkCALayerCanvas::Make` factory encapsulates device creation internally, following the pattern established by `SkSVGCanvas::Make` for non-pixel backends.

**Usage example (Objective-C++):**

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
auto canvas = SkCALayerCanvas::Make(SkISize::Make(320, 480), rootLayer);

// Draw
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

### 6.1 Tier 1: Unit Tests (`tests/CALayerDeviceTest.mm`)

Unit tests verify the structural output of the device — that drawing operations produce the expected CALayer tree with the correct properties. These tests do **not** compare pixels; they inspect the layer hierarchy directly.

**Framework:** Skia's `DEF_TEST` / `REPORTER_ASSERT` macros from `tests/Test.h`.

**Approach:** Create an `SkCALayerDevice`, wrap it in an `SkCanvas`, issue draw commands, then inspect the `rootLayer()` sublayer tree. This is analogous to `SVGDeviceTest.cpp`, which creates an `SkSVGDevice`, draws into it, then parses the resulting SVG XML to verify structure.

**Platform requirement:** Tests must run on macOS/iOS because they instantiate real `CALayer` objects. On other platforms, the tests should be conditionally compiled out (guarded by `#ifdef SK_BUILD_FOR_MAC` or `#ifdef SK_BUILD_FOR_IOS`).

### 6.2 Tier 2: GM Tests (`gm/calayer_rect.mm`)

GM (Golden Master) tests produce visual output that can be compared against reference images. For the CALayer backend, the GM test will:

1. Draw a known pattern using `SkCanvas` on the CALayer device.
2. Render the resulting `CALayer` tree into a `CGContext` bitmap (via `CALayer.renderInContext:`).
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

Write the test file `tests/CALayerDeviceTest.mm` with all key scenario tests from Section 6.4. The tests will reference `SkCALayerCanvas::Make()`, which does not yet exist — they will fail to compile (or link), establishing the "red" state.

- [ ] Create `tests/CALayerDeviceTest.mm` with platform guard (`#if defined(SK_BUILD_FOR_MAC) || defined(SK_BUILD_FOR_IOS)`)
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
- [ ] Create `src/calayer/SkCALayerConvert.h/.cpp` — pure C++ helpers (compiled as standard C++, no `-fobjc-arc`):
  - `SkCALayerCreateCGColor()`: `SkColor4f` → `CGColorRef` in sRGB color space
  - `SkCALayerMatrixHasPerspective()`: validate `SkMatrix` for perspective components
- [ ] Create `src/calayer/SkCALayerDevice.mm` — ObjC++ implementation (compiled with `-fobjc-arc`):
  - `Make(size, rootLayer)` factory: constructs `SkImageInfo::MakeUnknown(w, h)` + default `SkSurfaceProps`, returns `nullptr` on non-Apple platforms, debug-asserts main thread
  - Constructor(`SkImageInfo`, `SkSurfaceProps`, `CALayer*`): passes info/props to `SkClipStackDevice`, `CFRetain` the root layer
  - Destructor: `CFRelease` the root layer reference
  - `drawRect()`: perspective check, `CAShapeLayer` creation with `anchorPoint=(0,0)`, affine transform, paint application
  - `applyPaintToShapeLayer()`: Fill/Stroke/StrokeAndFill dispatch, sRGB `CGColor` conversion, stroke properties
  - All other draw methods: empty no-op bodies
- [ ] Create `src/calayer/BUILD.gn` — GN library target with Apple-only compilation, `-fobjc-arc` for `.mm` only
- [ ] Verify all Phase 1 unit tests pass (green)

**Done when:** All unit tests from Phase 1 pass on macOS. The device can render filled and stroked rectangles as `CAShapeLayer` sublayers.

### Phase 3: GM Test — 1 day

Write a GM test that renders a known rect pattern and verifies visual output.

- [ ] Create `gm/calayer_rect.mm` with `DEF_SIMPLE_GM` or class-based GM
- [ ] Implement CALayer-to-SkBitmap conversion helper (via `CALayer.renderInContext:` → `CGBitmapContext` → `SkBitmap`)
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
| Objective-C++ compilation complexity in GN | Low | Med | Skia's GN build system already handles `.mm` files for Apple ports. Follow existing patterns in `src/ports/` and `tools/skottie_ios_app/`. |
| Memory management (ARC vs. manual retain/release) across C++/ObjC boundary | Med | High | Use `CFTypeRef` (`void*`) with explicit `CFRetain`/`CFRelease` in the C++ layer. The `.mm` implementation file uses ARC internally. This pattern is established in `src/utils/mac/`. **Critical detail:** when the `.mm` file is compiled with `-fobjc-arc`, the compiler automatically inserts `objc_retain`/`objc_release` calls for Objective-C pointer variables. If C++ code (in `.h` or `.cpp`) holds an ObjC object pointer via `void*`, ARC cannot track that reference. The current design mitigates this by: (1) storing `CALayer*` as `void*` with manual `CFRetain`/`CFRelease`; (2) ensuring all `CAShapeLayer*` temporaries are created and consumed within a single ARC-managed `.mm` method scope (retained by `addSublayer:` before the method returns). Reviewers should verify that no ObjC pointer escapes an `.mm` scope into C++ storage without an explicit `CFRetain`. |

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
| 1.1 | 2026-03-23 | Chason Tang | Address review feedback: fix constructor signature (SkImageInfo+SkSurfaceProps); add perspective matrix validation; specify sRGB color space for CGColor; document anchorPoint/position/bounds setup; clarify iOS Y-down vs macOS Y-up; add applyPaintToShapeLayer pseudocode; separate ObjC/C++ source files; replace test scenario 7 with device-level partial-clip test; add perspective rejection test; document frame lifecycle and sublayer accumulation |
| 1.0 | 2026-03-20 | Chason Tang | Initial version |
