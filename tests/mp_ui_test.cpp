// Headless self-check for the lobby IP-field editor (src/ui/menu_util.hpp).
// No raylib, no InitWindow — pure string logic, runs anywhere.
#include "../src/ui/menu_util.hpp"
#include <cassert>
#include <cstring>

using game::ui::ip_edit;

int main() {
    char buf[8] = "";

    // typing digits and dots appends
    assert(ip_edit(buf, sizeof(buf), '1', false) && strcmp(buf, "1") == 0);
    assert(ip_edit(buf, sizeof(buf), '9', false) && strcmp(buf, "19") == 0);
    assert(ip_edit(buf, sizeof(buf), '.', false) && strcmp(buf, "19.") == 0);

    // disallowed chars (letters, symbols) are rejected, buffer unchanged
    assert(!ip_edit(buf, sizeof(buf), 'a', false));
    assert(!ip_edit(buf, sizeof(buf), ' ', false));
    assert(strcmp(buf, "19.") == 0);

    // ch==0 with no backspace is a no-op
    assert(!ip_edit(buf, sizeof(buf), 0, false));

    // backspace removes the last char
    assert(ip_edit(buf, sizeof(buf), 0, true) && strcmp(buf, "19") == 0);

    // backspace on empty buffer is a no-op (not an underflow)
    char empty[4] = "";
    assert(!ip_edit(empty, sizeof(empty), 0, true));
    assert(strcmp(empty, "") == 0);

    // capacity is respected: cap=4 -> at most 3 chars + NUL, never overruns
    char tiny[4] = "";
    assert(ip_edit(tiny, sizeof(tiny), '1', false));
    assert(ip_edit(tiny, sizeof(tiny), '2', false));
    assert(ip_edit(tiny, sizeof(tiny), '3', false));
    assert(strcmp(tiny, "123") == 0);
    assert(!ip_edit(tiny, sizeof(tiny), '4', false)); // full: rejected, no overrun
    assert(strcmp(tiny, "123") == 0);

    return 0;
}
