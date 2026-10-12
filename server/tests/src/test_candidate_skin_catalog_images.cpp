#include "tests/includes/test_framework.h"

#include "skin/candidate_skin_catalog.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
constexpr const char *kManifestHead = R"(schema_version = 1
id = "art"
name = "Art"
version = "1.0.0"
base = "fluent"

[supports]
layouts = ["horizontal", "vertical"]
themes = ["dark", "light"]
)";

// `candidateWindow` is appended verbatim after the head, so each case owns the [candidate_window] tables.
std::filesystem::path WriteSkin(const std::wstring &leaf, const std::string &candidateWindow)
{
    namespace fs = std::filesystem;
    const fs::path skins_root =
        fs::temp_directory_path() / (L"msime-skin-images-" + std::to_wstring(GetCurrentProcessId())) / leaf / L"skins";
    std::error_code ec;
    fs::remove_all(skins_root, ec);
    fs::create_directories(skins_root / L"art" / L"assets", ec);
    REQUIRE(!ec);
    for (const wchar_t *image : {L"assets\\character.png", L"assets\\paper.png"})
    {
        std::ofstream file(skins_root / L"art" / image, std::ios::binary | std::ios::trunc);
        REQUIRE(file.is_open());
    }
    std::ofstream manifest(skins_root / L"art" / L"skin.toml", std::ios::binary | std::ios::trunc);
    REQUIRE(manifest.is_open());
    manifest << kManifestHead << candidateWindow;
    return skins_root;
}

void RemoveSkin(const std::filesystem::path &root)
{
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

bool LoadFails(const std::wstring &leaf, const std::string &candidateWindow)
{
    const auto root = WriteSkin(leaf, candidateWindow);
    std::string error;
    const bool failed = !CandidateSkinCatalog::Load(root, "art", &error).has_value() && !error.empty();
    RemoveSkin(root);
    return failed;
}
} // namespace

// Every image key is optional: a skin without them has no decoration, no background image and keeps the
// base theme's corner radius.
TEST_CASE(candidate_skin_catalog_image_tables_are_optional)
{
    const auto root = WriteSkin(L"plain", "[candidate_window]\n");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->decorationImage.empty());
    REQUIRE_EQ(package->decorationTopDip, 0.0);
    REQUIRE_EQ(package->decorationWidthDip, 0.0);
    REQUIRE(!package->cornerRadiusDip.has_value());
    REQUIRE(package->backgroundImage.empty());
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_reads_background_corner_and_decoration_placement)
{
    const auto root = WriteSkin(L"valid", R"(
[candidate_window]
corner_radius_dip = 0

[candidate_window.decoration]
image = "assets/character.png"
top_inset_dip = 88
width_dip = 136
align = "left"

[candidate_window.background]
image = "assets/paper.png"
fit = "contain"
opacity = 0.5
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->decorationImage, std::string("assets/character.png"));
    REQUIRE_EQ(package->decorationTopDip, 88.0);
    REQUIRE_EQ(package->decorationWidthDip, 136.0);
    REQUIRE_EQ(package->decorationAlign, std::string("left"));
    // An explicit 0 means square corners, which is different from "not set".
    REQUIRE(package->cornerRadiusDip.has_value());
    REQUIRE_EQ(*package->cornerRadiusDip, 0.0);
    REQUIRE_EQ(package->backgroundImage, std::string("assets/paper.png"));
    REQUIRE_EQ(package->backgroundFit, std::string("contain"));
    REQUIRE_EQ(package->backgroundOpacity, 0.5);
    RemoveSkin(root);
}

TEST_CASE(candidate_skin_catalog_rejects_incomplete_or_invalid_image_tables)
{
    const std::string window = "[candidate_window]\n";
    const std::string decoration = "[candidate_window.decoration]\n";
    const std::string sized = "top_inset_dip = 88\nwidth_dip = 136\n";
    const std::string character = "image = \"assets/character.png\"\n";
    const std::string background = "[candidate_window.background]\n";
    const std::string paper = "image = \"assets/paper.png\"\n";

    // A decoration needs its image and both sizes.
    REQUIRE(LoadFails(L"deco-empty", window + decoration));
    REQUIRE(LoadFails(L"deco-no-image", window + decoration + sized));
    REQUIRE(LoadFails(L"deco-no-top", window + decoration + character + "width_dip = 136\n"));
    REQUIRE(LoadFails(L"deco-no-width", window + decoration + character + "top_inset_dip = 88\n"));
    REQUIRE(LoadFails(L"deco-missing", window + decoration + sized + "image = \"assets/missing.png\"\n"));
    REQUIRE(LoadFails(L"deco-escape", window + decoration + sized + "image = \"../x.png\"\n"));
    REQUIRE(LoadFails(L"deco-align", window + decoration + sized + character + "align = \"top\"\n"));

    REQUIRE(LoadFails(L"radius", "[candidate_window]\ncorner_radius_dip = 40\n"));
    REQUIRE(LoadFails(L"radius-type", "[candidate_window]\ncorner_radius_dip = \"8px\"\n"));

    REQUIRE(LoadFails(L"bg-no-image", window + background + "fit = \"cover\"\n"));
    REQUIRE(LoadFails(L"bg-fit", window + background + paper + "fit = \"tile\"\n"));
    REQUIRE(LoadFails(L"bg-opacity", window + background + paper + "opacity = 2\n"));
    REQUIRE(LoadFails(L"bg-missing", window + background + "image = \"assets/missing.png\"\n"));
}

TEST_CASE(candidate_skin_catalog_reads_toolbar_colors_and_corner_radius)
{
    // The rgba() values end in `)"`, so the raw string needs its own delimiter.
    const auto root = WriteSkin(L"toolbar", R"toml([candidate_window]

[toolbar]
corner_radius_dip = 12

[toolbar.dark]
border = "rgba(224, 138, 168, 0.38)"
handle = "#e08aa8"

[toolbar.light]
background = "#fff7fa"
divider = "rgba(176, 80, 110, 0.22)"
icon = "#3a2a30"
hover = "rgba(196, 92, 122, 0.10)"
)toml");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE(package->toolbarCornerRadiusDip.has_value());
    REQUIRE_EQ(*package->toolbarCornerRadiusDip, 12.0);
    REQUIRE_EQ(package->toolbarDark.border, std::string("rgba(224, 138, 168, 0.38)"));
    REQUIRE_EQ(package->toolbarDark.handle, std::string("#e08aa8"));
    REQUIRE(package->toolbarDark.background.empty());
    REQUIRE_EQ(package->toolbarLight.background, std::string("#fff7fa"));
    REQUIRE_EQ(package->toolbarLight.divider, std::string("rgba(176, 80, 110, 0.22)"));
    REQUIRE_EQ(package->toolbarLight.icon, std::string("#3a2a30"));
    REQUIRE_EQ(package->toolbarLight.hover, std::string("rgba(196, 92, 122, 0.10)"));
    REQUIRE(package->toolbarLight.handle.empty());
    RemoveSkin(root);

    // Without a [toolbar] table the toolbar keeps the base skin entirely.
    const auto plainRoot = WriteSkin(L"toolbar-absent", "[candidate_window]\n");
    const auto plain = CandidateSkinCatalog::Load(plainRoot, "art", &error);
    REQUIRE(plain.has_value());
    REQUIRE(!plain->toolbarCornerRadiusDip.has_value());
    REQUIRE(plain->toolbarDark.handle.empty());
    REQUIRE(plain->toolbarLight.background.empty());
    RemoveSkin(plainRoot);
}

TEST_CASE(candidate_skin_catalog_rejects_invalid_toolbar_tables)
{
    const std::string window = "[candidate_window]\n";
    REQUIRE(LoadFails(L"tb-not-table", window + "[[toolbar]]\n"));
    REQUIRE(LoadFails(L"tb-theme-not-table", window + "[toolbar]\ndark = \"pink\"\n"));
    REQUIRE(LoadFails(L"tb-color-type", window + "[toolbar.dark]\nhandle = 7\n"));
    // Toolbar colours are pasted into the WebView2 page's CSS, so anything that could end the declaration is refused.
    REQUIRE(LoadFails(L"tb-color-inject", window + "[toolbar.light]\nicon = \"red; } body { display: none\"\n"));
    REQUIRE(LoadFails(L"tb-radius", window + "[toolbar]\ncorner_radius_dip = 40\n"));
    REQUIRE(LoadFails(L"tb-radius-type", window + "[toolbar]\ncorner_radius_dip = \"8px\"\n"));
}

TEST_CASE(candidate_skin_catalog_reads_v2_features)
{
    namespace fs = std::filesystem;
    const fs::path skins_root =
        fs::temp_directory_path() / (L"msime-skin-v2-" + std::to_wstring(GetCurrentProcessId())) / L"skins";
    std::error_code ec;
    fs::remove_all(skins_root, ec);
    fs::create_directories(skins_root / L"art" / L"assets", ec);
    REQUIRE(!ec);
    for (const wchar_t *image : {L"assets\\border.png", L"assets\\char.png"})
    {
        std::ofstream file(skins_root / L"art" / image, std::ios::binary | std::ios::trunc);
        REQUIRE(file.is_open());
    }
    std::ofstream manifest(skins_root / L"art" / L"skin.toml", std::ios::binary | std::ios::trunc);
    REQUIRE(manifest.is_open());
    manifest << R"toml(schema_version = 2
id = "art"
name = "Art"
version = "2.0.0"
base = "fluent"

[supports]
layouts = ["horizontal"]
themes = ["dark"]

[candidate_window]
[candidate_window.border_image]
source = "assets/border.png"
slice_dip = [16, 44, 16, 0]
stretch_dip = [16, 90, 16, 0]
width_dip = [16, 44, 16, 0]
fill = true

line_gap_dip = 6

[candidate_window.content_insets_dip]
top = 4
right = 130
bottom = 14
left = 55

[candidate_window.outer_insets_dip]
top = 87
right = 16
bottom = 4
left = 0

[[candidate_window.decorations]]
image = "assets/char.png"
anchor = "right"
offset_x_dip = 10
offset_y_dip = -15
width_dip = 149
height_dip = 161
z_index = 3
)toml";
    manifest.close();

    std::string error;
    const auto package = CandidateSkinCatalog::Load(skins_root, "art", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->schemaVersion, 2);
    REQUIRE(package->borderImage.has_value());
    REQUIRE_EQ(package->borderImage->source, std::string("assets/border.png"));
    REQUIRE_EQ(package->borderImage->sliceDip.size(), static_cast<size_t>(4));
    REQUIRE_EQ(package->borderImage->sliceDip[1], 44.0);
    // stretch_dip 与 slice_dip 分开存：前者才是可拉伸区，渲染时优先用它。
    REQUIRE_EQ(package->borderImage->stretchDip.size(), static_cast<size_t>(4));
    REQUIRE_EQ(package->borderImage->stretchDip[1], 90.0);
    REQUIRE(package->borderImage->fill);

    REQUIRE(package->contentInsetsDip.has_value());
    REQUIRE_EQ(package->contentInsetsDip->left, 55.0);
    REQUIRE_EQ(package->contentInsetsDip->right, 130.0);

    REQUIRE(package->lineGapDip.has_value());
    REQUIRE_EQ(*package->lineGapDip, 6.0);

    REQUIRE(package->outerInsetsDip.has_value());
    REQUIRE_EQ(package->outerInsetsDip->top, 87.0);
    REQUIRE_EQ(package->outerInsetsDip->right, 16.0);

    REQUIRE_EQ(package->freeDecorations.size(), static_cast<size_t>(1));
    REQUIRE_EQ(package->freeDecorations[0].anchor, std::string("right"));
    REQUIRE_EQ(package->freeDecorations[0].widthDip, 149.0);
    REQUIRE_EQ(package->freeDecorations[0].zIndex, 3);

    fs::remove_all(skins_root, ec);
}
