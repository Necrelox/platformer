#include "core/api.hpp"
#include <cassert>

using namespace game;

static Player make_player(Vector2 pos) {
    Player p;
    p.pos = pos;
    p.alive = true;
    return p;
}

// Construct items directly instead of via spawn::item — keeps this test linkable against
// just items.cpp, not the whole core (spawn:: lives in core/game.cpp).
static Item& make_item(World& w, Vector2 pos, ItemType type) {
    Item it; it.pos = pos; it.type = type;
    return w.items.emplace_back(it);
}

int main() {
    // --- Heart pickup increases hp when below maxHp ---
    {
        World w;
        Player p = make_player({ 0, 0 });
        p.hp = 1; p.maxHp = 3;
        w.players.push_back(p);
        Item& it = make_item(w, { 0, 0 }, ItemType::Heart); // overlaps player exactly
        items::update(w, 0.016f);
        assert(w.players[0].hp == 2);
        assert(w.players[0].maxHp == 3);
        assert(w.items[0].alive == false); // consumed
        (void)it;
    }

    // --- Heart pickup at full hp grants bonus heart (maxHp++, hp++) ---
    {
        World w;
        Player p = make_player({ 0, 0 });
        p.hp = 3; p.maxHp = 3;
        w.players.push_back(p);
        make_item(w, { 0, 0 }, ItemType::Heart);
        items::update(w, 0.016f);
        assert(w.players[0].maxHp == 4);
        assert(w.players[0].hp == 4);
    }

    // --- RapidFire pickup sets weapon ---
    {
        World w;
        Player p = make_player({ 0, 0 });
        w.players.push_back(p);
        make_item(w, { 0, 0 }, ItemType::RapidFire);
        items::update(w, 0.016f);
        assert(w.players[0].weapon == Weapon::Rapid);
    }

    // --- DoubleShot pickup sets weapon ---
    {
        World w;
        Player p = make_player({ 0, 0 });
        w.players.push_back(p);
        make_item(w, { 0, 0 }, ItemType::DoubleShot);
        items::update(w, 0.016f);
        assert(w.players[0].weapon == Weapon::Double);
    }

    // --- Invincibility pickup sets flag+timer, then expires after >6s ---
    {
        World w;
        Player p = make_player({ 0, 0 });
        w.players.push_back(p);
        make_item(w, { 0, 0 }, ItemType::Invincibility);
        items::update(w, 0.016f);
        assert(w.players[0].invincible == true);
        assert(w.players[0].powerTimer > 0);

        // Run update() repeatedly for more than 6s of simulated time.
        float elapsed = 0.016f;
        while (elapsed < 6.5f) {
            items::update(w, 0.05f);
            elapsed += 0.05f;
        }
        assert(w.players[0].invincible == false);
        assert(w.players[0].powerTimer == 0);
    }

    // --- Non-overlapping item is not picked up ---
    {
        World w;
        Player p = make_player({ 1000, 1000 });
        w.players.push_back(p);
        make_item(w, { 0, 0 }, ItemType::Heart);
        float before_t = w.items[0].t;
        items::update(w, 0.1f);
        assert(w.items[0].alive == true);
        assert(w.items[0].t > before_t); // bob timer still advances
    }

    return 0;
}
