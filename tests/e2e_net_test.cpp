// End-to-end multiplayer over real ENet loopback, headless (no window). Two real Game
// instances: a Host running the actual sim (tick) + broadcast, and a Client generating its
// own world from the same seed and applying the host's snapshot via sync_client. Proves the
// whole chain: connect -> lobby -> start -> generation parity -> snapshot -> client mirrors host.
#include "core/api.hpp"
#include <cassert>
#include <cstdio>
#include <cmath>

using namespace game;

static void pump(Game& h, Game& c, int n) { for (int i = 0; i < n; ++i) { net::poll(h); net::poll(c); } }

int main() {
    const uint16_t PORT = 45471;
    const uint64_t SEED = 99999ull;

    Game h, c;

    h.net.role = NetRole::Host;
    assert(net::start_host(PORT, MAX_PLAYERS));
    h.net.lobby = Lobby{};
    h.net.lobby.slots[0].used = true;
    h.net.lobby.localSlot = 0;
    h.net.lobby.seed = SEED;

    c.net.role = NetRole::Client;
    assert(net::start_client("127.0.0.1", PORT));

    // 1) connect handshake
    for (int i = 0; i < 400 && !c.net.connected; ++i) pump(h, c, 1);
    assert(c.net.connected);
    pump(h, c, 20);
    assert(h.net.lobby.slots[1].used);      // host assigned the client a slot
    assert(c.net.lobby.localSlot == 1);

    // 2) host starts the game; client enters Playing via the StartGame message
    net::send_start(h);
    start_game_from_lobby(h);               // host spawns players for slots 0 & 1, seed SEED
    for (int i = 0; i < 400 && c.mode != Mode::Playing; ++i) pump(h, c, 1);
    assert(c.mode == Mode::Playing);
    assert(h.world.players.size() == 2);
    assert(c.world.players.size() == 2);

    // 3) host simulates: move host player (slot 0) right for ~2s
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 120; ++i) { h.inputs[0].right = true; tick(h, dt); }
    float hx0 = h.world.players[0].pos.x;
    assert(hx0 > 200.0f);                   // it actually moved

    // 4) broadcast one authoritative snapshot and deliver it
    net::broadcast_snapshot(h);
    pump(h, c, 20);

    // 5) client converges toward the snapshot (eased apply over many frames)
    for (int i = 0; i < 400; ++i) net::sync_client(c, dt);

    float cx0 = -1;
    for (const auto& p : c.world.players) if (p.id == 0) cx0 = p.pos.x;
    printf("host p0 x=%.1f  client p0 x=%.1f  | client players=%zu enemies=%zu platforms=%zu\n",
           hx0, cx0, c.world.players.size(), c.world.enemies.size(), c.world.platforms.size());

    assert(cx0 > 0);                        // client knows player 0
    assert(std::fabs(cx0 - hx0) < 5.0f);    // client mirrored the host's authoritative position
    // client generated its own world from the seed and passed sync_client's desync guard:
    assert(!c.world.enemies.empty());

    net::shutdown();
    puts("e2e_net_test OK");
    return 0;
}
