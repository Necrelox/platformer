// Self-check for src/net/net.cpp — headless transport loopback (host + client both run in
// this one process; net.cpp keeps host/client transport state in separate file-local statics,
// so this is safe). No InitWindow, no InitAudioDevice.
//
// This validates transport/protocol only — it CANNOT catch host/client worldgen divergence;
// that's what sync_client's runtime desync guard (size check + TraceLog + skip) is for.
//
// Build+run:
//   g++ -std=c++26 -Isrc -Ibuild/_deps/enet-src/include $(pkg-config --cflags raylib) \
//       tests/net_test.cpp src/net/net.cpp src/core/game.cpp src/world/worldgen.cpp \
//       src/platforms/platforms.cpp src/combat/combat.cpp src/items/items.cpp src/fx/fx.cpp \
//       build/_deps/enet-build/libenet.a -lraylib -lm -o /tmp/net_test && /tmp/net_test
#include "core/api.hpp"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <thread>

using namespace game;

int main() {
    Game gHostGame;
    gHostGame.net.role = NetRole::Host;
    gHostGame.net.lobby.slots[0].used = true; // ponytail: normally the UI does this; mirrored here
    gHostGame.net.lobby.localSlot = 0;

    Game gClientGame;
    gClientGame.net.role = NetRole::Client;

    assert(net::start_host(45460, 4));
    assert(net::start_client("127.0.0.1", 45460));

    // Drain both sides until the client sees itself connected and the host sees one peer.
    for (int i = 0; i < 40 && !(gClientGame.net.connected && net::peer_count() == 1); ++i) {
        net::poll(gHostGame);
        net::poll(gClientGame);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(gClientGame.net.connected);
    assert(net::peer_count() == 1);
    assert(gClientGame.net.lobby.localSlot >= 1 && gClientGame.net.lobby.localSlot < MAX_PLAYERS);
    printf("connected: localSlot=%d peer_count=%d\n", gClientGame.net.lobby.localSlot, net::peer_count());

    // Client -> host input: one edge (jumpSeq) plus a held key, must arrive intact.
    int slot = gClientGame.net.lobby.localSlot;
    gClientGame.inputs[slot].left = true;
    gClientGame.inputs[slot].jumpSeq = 7;
    net::send_input(gClientGame);

    bool gotIt = false;
    for (int i = 0; i < 40 && !gotIt; ++i) {
        net::poll(gHostGame);
        net::poll(gClientGame);
        gotIt = gHostGame.inputs[slot].left && gHostGame.inputs[slot].jumpSeq == 7;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(gotIt);
    printf("host received input: left=%d jumpSeq=%u\n", gHostGame.inputs[slot].left, gHostGame.inputs[slot].jumpSeq);

    // ---- snapshot round-trip: host encodes real World state, client decodes + applies it ----
    // (this is the riskiest untested path — a swapped/dropped field here would fail silently
    // until two real machines connected)
    {
        World& hw = gHostGame.world;
        hw.players.push_back(Player{});
        hw.players[0].id = 0;
        hw.players[0].pos = { 100.f, 50.f };
        hw.players[0].hp = 2; hw.players[0].maxHp = 3;
        hw.players[0].distance = 500.f;

        Enemy en; en.pos = { 200.f, 60.f }; en.hp = 1; en.state = EnemyState::Aggro; en.alive = true;
        hw.enemies.push_back(en);

        Item it; it.alive = false; // already picked up
        hw.items.push_back(it);

        Platform pf; pf.rect = { 300.f, 70.f, 48.f, 24.f }; pf.active = false; pf.triggered = true;
        hw.platforms.push_back(pf);

        Projectile pr; pr.pos = { 400.f, 80.f }; pr.vel = { 700.f, 0.f }; pr.fromPlayer = true;
        hw.projectiles.push_back(pr);
        hw.genCol = 64;

        // Client pre-seeded with matching-COUNT (but stale) vectors: satisfies the desync guard
        // and sidesteps depending on real worldgen output for this synthetic-data check.
        World& cw = gClientGame.world;
        cw.players.push_back(Player{});
        cw.enemies.push_back(Enemy{});
        cw.items.push_back(Item{});
        cw.platforms.push_back(Platform{});
        cw.genCol = 64;

        net::broadcast_snapshot(gHostGame); // first call always sends (see lastSend ponytail note)

        bool decoded = false;
        for (int i = 0; i < 40 && !decoded; ++i) {
            // enet_host_broadcast only QUEUES the packet — it isn't put on the wire until the
            // sending host's own enet_host_service runs, so the host side must be polled too
            // (the real game always does this every frame regardless of role).
            net::poll(gHostGame);
            net::poll(gClientGame);
            net::sync_client(gClientGame, 1.0f); // dt=1s -> ease clamps to 1 (snap fully in one call)
            decoded = cw.players.size() == 1 && std::fabs(cw.players[0].pos.x - 100.f) < 1.0f;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(decoded);
        assert(cw.players[0].hp == 2 && cw.players[0].maxHp == 3);
        assert(std::fabs(cw.players[0].distance - 500.f) < 0.01f);
        assert(cw.enemies[0].alive && cw.enemies[0].state == EnemyState::Aggro && cw.enemies[0].hp == 1);
        assert(!cw.items[0].alive);
        assert(!cw.platforms[0].active && cw.platforms[0].triggered);
        assert(std::fabs(cw.platforms[0].rect.x - 300.f) < 1.0f);
        assert(cw.projectiles.size() == 1 && std::fabs(cw.projectiles[0].pos.x - 400.f) < 1.0f);
        puts("snapshot round-trip OK");
    }

    net::shutdown();
    assert(!net::is_active());

    puts("net_test OK");
    return 0;
}
