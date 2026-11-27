#include "ui/frame/draw-brand/draw-brand.hpp"
#include "ui/types/canvas/canvas.hpp"
#include "ui/types/ui-state/ui-state.hpp"

namespace ui {
void DrawBrand(Canvas& c, const UiState& s) {
    const float x = s.window_pos.x + 20, y = s.window_pos.y + 6;
    c.draw->AddCircleFilled({x + 4, y + 6}, 3, c.C(118,226,187));
    c.Text(x + 15, y, "RR Studio  /  Internal", c.C(205,228,223));
}
}
