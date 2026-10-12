#pragma once

#include "msimeui/Layout.h"

#include <string>
#include <vector>

namespace msimeui
{
class DeviceResources;
}

// v2 皮肤的自由挂件层。它不参与布局（测量恒为 0），由父级摆到卡片那一格，
// 内部按 anchor + offset 把挂件画到卡片边界之外，所以窗口必须按 outer_insets_dip 多留出空间。
class CandidateSkinOverlay : public msimeui::Visual
{
  public:
    struct Item
    {
        std::wstring path;
        // top-left / top-right / bottom-left / bottom-right / left / right / top / bottom / center
        std::string anchor = "right";
        // 相对锚点的平移量，正数向右、向下。
        float offsetXDip = 0.0f;
        float offsetYDip = 0.0f;
        float widthDip = 0.0f;
        float heightDip = 0.0f;
    };

    void SetItems(std::vector<Item> items);

    msimeui::SizeF Measure(const msimeui::SizeF &availableSize) override;
    void Arrange(const msimeui::RectF &finalRect) override;
    void Render(msimeui::DeviceResources &deviceResources) override;

  private:
    std::vector<Item> items_;
    msimeui::RectF bounds_{};
};
