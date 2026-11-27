#include "ui/frame/draw-tabs/draw-tabs.hpp"
#include "ui/color/pack-color/pack-color.hpp"
#include "ui/state/theme/theme.hpp"
#include "ui/types/canvas/canvas.hpp"
#include "ui/types/tab/tab.hpp"
#include "ui/types/ui-state/ui-state.hpp"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "imgui.h"

namespace ui
{
    void DrawTabs(Canvas& c, UiState& s)
    {
        static const char* names[] = {
            "Players", "Aim", "Visuals", "World", "Style",
#ifndef RR_INTERNAL
            "debug"
#endif
        };
        const float x = s.window_pos.x + 20;
        const float y = s.window_pos.y + 23;
        const int n = static_cast<int>(sizeof(names) / sizeof(names[0]));
        const float w = InnerSize.x / static_cast<float>(n);
        for (int i = 0; i < n; ++i)
        {
            const float cx = x + w * i + w * 0.5f;
            const float text_w = ImGui::CalcTextSize(names[i]).x;
            const float text_x = cx - text_w * 0.5f;
            const Tab tab = static_cast<Tab>(i);
            ImGui::SetCursorScreenPos(ImVec2(x + w * i, y));
            ImGui::PushID(i);
            ImGui::InvisibleButton("tab", ImVec2(w, 19.0f));
            if (ImGui::IsItemClicked())
                { if (s.tab != tab) s.content_scroll = s.content_height = 0; s.tab = tab; }
            const bool selected = s.tab == tab;
            if (selected || ImGui::IsItemHovered())
                c.draw->AddRectFilled({x+w*i+2,y},{x+w*(i+1)-2,y+19},
                    selected ? c.C(32,68,63) : c.C(26,49,56),6);
            c.Text(text_x, y + 2, names[i], selected ? c.C(184,255,226) : c.C(175,197,196));
            ImGui::PopID();
        }
    }
}
