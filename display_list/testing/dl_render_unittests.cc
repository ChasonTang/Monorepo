#include <tuple>
#include <vector>

#include "display_list/dl_blend_mode.h"
#include "display_list/dl_color.h"
#include "display_list/testing/dl_render_test.h"
#include "display_list/testing/dl_render_test_env.h"

#include "gtest/gtest.h"

namespace flutter {
namespace testing {

// 颜色/全屏 OPs (mirrors flutter/.../dl_rendering_unittests.cc:2910-2958).
static const std::vector<OpCase>& ColorOps() {
  static const std::vector<OpCase> kOps = {
      {"DrawPaint",
       [](const RenderContext& ctx) {
         DlPaint p = ctx.paint;
         p.setColor(DlColor::kMagenta());
         ctx.canvas->DrawPaint(p);
       }},
      {"DrawOpaqueColor",
       [](const RenderContext& ctx) {
         ctx.canvas->DrawColor(DlColor::kMagenta(), DlBlendMode::kSrcOver);
       }},
      {"DrawAlphaColor",
       [](const RenderContext& ctx) {
         ctx.canvas->DrawColor(DlColor(0x7FFF00FF), DlBlendMode::kSrcOver);
       }},
  };
  return kOps;
}

// 基线 Variations: identity, no setup.
static const std::vector<VariationCase>& BaselineVariations() {
  static const std::vector<VariationCase> kVariations = {
      {"Defaults", [](const RenderContext&) {}},
  };
  return kVariations;
}

class DlRenderTest : public ::testing::TestWithParam<
                         std::tuple<VariationCase, OpCase>> {};

TEST_P(DlRenderTest, Smoke) {
  const auto& variation = std::get<0>(GetParam());
  const auto& op = std::get<1>(GetParam());
  RenderEnv env;
  sk_sp<SkSurface> surface = env.RenderToSkia(variation, op);
  ASSERT_NE(surface, nullptr);
  EXPECT_TRUE(RenderEnv::HasNonClearPixel(surface));
}

INSTANTIATE_TEST_SUITE_P(
    Baseline_Color,
    DlRenderTest,
    ::testing::Combine(::testing::ValuesIn(BaselineVariations()),
                       ::testing::ValuesIn(ColorOps())),
    [](const ::testing::TestParamInfo<DlRenderTest::ParamType>& info) {
      return std::get<0>(info.param).name + "_" + std::get<1>(info.param).name;
    });

}  // namespace testing
}  // namespace flutter

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
