// Headless self-check for the menu selection-index wrap logic (src/ui/menu_util.hpp).
// No raylib, no InitWindow — pure arithmetic, runs anywhere.
#include "../src/ui/menu_util.hpp"
#include <cassert>

using game::ui::wrap_index;

int main() {
    // no movement stays put
    assert(wrap_index(0, 0, 4) == 0);
    assert(wrap_index(2, 0, 4) == 2);

    // forward wrap past the end
    assert(wrap_index(3, 1, 4) == 0);
    assert(wrap_index(0, 1, 4) == 1);

    // backward wrap past the start
    assert(wrap_index(0, -1, 4) == 3);
    assert(wrap_index(1, -1, 4) == 0);

    // multi-step and full-loop stability
    assert(wrap_index(0, 4, 4) == 0);
    assert(wrap_index(0, -4, 4) == 0);
    assert(wrap_index(2, 5, 4) == 3); // 2+5=7 -> 7%4=3

    return 0;
}
