#include "core/api.hpp"

// Phase 6 — items/power-ups: bob timer, pickup detection, effect application, power-up expiry.
namespace game::items {

static constexpr float INVINCIBILITY_DURATION = 6.0f; // s

static bool overlaps(Vector2 aPos, Vector2 aSize, Vector2 bPos, Vector2 bSize) {
    Rectangle a{ aPos.x, aPos.y, aSize.x, aSize.y };
    Rectangle b{ bPos.x, bPos.y, bSize.x, bSize.y };
    return CheckCollisionRecs(a, b);
}

static void apply(Player& p, ItemType type) {
    switch (type) {
        case ItemType::Heart:
            if (p.hp < p.maxHp) p.hp += 1;
            else { p.maxHp += 1; p.hp += 1; } // bonus heart when already full
            break;
        case ItemType::RapidFire:
            p.weapon = Weapon::Rapid;
            break;
        case ItemType::DoubleShot:
            p.weapon = Weapon::Double;
            break;
        case ItemType::Invincibility:
            p.invincible = true;
            p.powerTimer = INVINCIBILITY_DURATION;
            break;
        case ItemType::Coin:
            break; // coins removed (served no purpose) — enum kept for net append-only compat

    }
}

void update(World& w, float dt) {
    // core::physics::step already decrements player.invuln (hit-invincibility).
    // powerTimer (item-granted invincibility) is ours to tick.
    for (auto& p : w.players) {
        if (!p.invincible) continue;
        p.powerTimer -= dt;
        if (p.powerTimer <= 0) { p.invincible = false; p.powerTimer = 0; }
    }

    for (auto& it : w.items) {
        if (!it.alive) continue;
        it.t += dt; // bob phase; render owns the visual offset

        for (auto& p : w.players) {
            if (!p.alive) continue;
            if (!overlaps(it.pos, it.size, p.pos, p.size)) continue;
            apply(p, it.type);
            it.alive = false;
            fx::emit(w, fx::Event::Pickup, { it.pos.x + it.size.x * 0.5f, it.pos.y + it.size.y * 0.5f });
            break; // item consumed, no need to check other players
        }
    }
}

} // namespace game::items
