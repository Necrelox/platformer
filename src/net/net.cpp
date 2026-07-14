#include "core/api.hpp"
#include <enet/enet.h>
#include <cstring>
#include <cmath>
#include <vector>

// Phases 8-10 — host-authoritative LAN transport over ENet.
//
// Wire format: fixed POD structs, sent as raw memcpy'd bytes (first byte always a MsgType).
// ponytail: no endian handling / versioning — every peer in this game is running the exact
// same compiled binary (LAN co-op, not a public server), so raw struct layout is identical
// on both ends. If this ever needs to talk to a different build/arch, add explicit
// field-by-field (de)serialization.
namespace game::net {

namespace {

// ---- protocol ----
enum class MsgType : uint8_t { Hello, Welcome, LobbyState, StartGame, Input, Snapshot };

constexpr enet_uint8 CH_RELIABLE   = 0;   // hello / lobby / start / input
constexpr enet_uint8 CH_SNAPSHOT   = 1;   // unreliable world snapshots
constexpr size_t     CHANNEL_COUNT = 2;
constexpr float      SNAPSHOT_HZ   = 30.0f;
constexpr float      SNAP_EASE_RATE = 15.0f; // 1/s — position lerp-toward-target rate (see sync_client)

#pragma pack(push, 1)
struct HelloMsg     { MsgType type; int32_t skin; uint8_t ready; char name[16]; };
struct WelcomeMsg    { MsgType type; int32_t slot; };
struct SlotWire      { uint8_t used, ready; int32_t skin; char name[16]; };
struct LobbyStateMsg { MsgType type; SlotWire slots[MAX_PLAYERS]; uint64_t seed; uint8_t started; };
struct StartGameMsg  { MsgType type; uint64_t seed; };
struct InputMsg      { MsgType type; InputState input; };

struct SnapHeader {
    MsgType  type;
    int32_t  genCol;
    uint16_t nPlayers, nEnemies, nItems, nPlatforms, nProjectiles;
};
struct PlayerWire {
    int32_t id;
    float   px, py, vx, vy;
    int32_t hp, maxHp;
    uint8_t facing, weapon;
    float   invuln;
    uint8_t alive, invincible;
    float   distance;
};
struct EnemyWire     { float px, py, vx, vy; int32_t hp; uint8_t state, facing, alive; };
struct ItemWire      { uint8_t alive; };
struct PlatformWire  { float rx, ry; uint8_t active, triggered; };
struct ProjectileWire{ float px, py, vx, vy; uint8_t fromPlayer; };
#pragma pack(pop)

struct LatestSnapshot {
    bool     valid  = false;
    int32_t  genCol = 0;
    std::vector<PlayerWire>     players;
    std::vector<EnemyWire>      enemies;
    std::vector<ItemWire>       items;
    std::vector<PlatformWire>   platforms;
    std::vector<ProjectileWire> projectiles;
};

// ---- transport state (file-local; each agent owns its own) ----
bool       gEnetInited = false;
ENetHost*  gHost        = nullptr; // non-null while role == Host
ENetHost*  gClient      = nullptr; // non-null while role == Client
ENetPeer*  gServerPeer  = nullptr; // client's peer to the host
LatestSnapshot gLatest;

bool ensure_enet_init() {
    if (gEnetInited) return true;
    if (enet_initialize() != 0) return false;
    gEnetInited = true;
    return true;
}

// NOTE: gHost and gClient are independent statics (a process is either Host or Client in the
// real game) — each close_* function tears down only its own side so that, e.g., a same-process
// test harness can run a host and a client together without one start_*() call nuking the other.
void close_host() {
    if (!gHost) return;
    // ponytail: hard-disconnect (no graceful drain) — fine for a LAN game tearing down.
    for (size_t i = 0; i < gHost->peerCount; ++i) {
        ENetPeer& peer = gHost->peers[i];
        if (peer.state != ENET_PEER_STATE_DISCONNECTED) enet_peer_disconnect_now(&peer, 0);
    }
    enet_host_destroy(gHost);
    gHost = nullptr;
}
void close_client() {
    if (!gClient) return;
    if (gServerPeer) enet_peer_disconnect_now(gServerPeer, 0);
    enet_host_destroy(gClient);
    gClient = nullptr; gServerPeer = nullptr;
}
void close_transport() {
    close_host();
    close_client();
    gLatest = LatestSnapshot{};
}

int slot_of(ENetPeer* peer) { return (int)(intptr_t)peer->data; }

void handle_host_connect(Game& g, ENetPeer* peer) {
    int slot = -1;
    for (int s = 1; s < MAX_PLAYERS; ++s)
        if (!g.net.lobby.slots[s].used) { slot = s; break; }
    if (slot < 0) { // lobby full — ponytail: silently drop, no reason code
        enet_peer_disconnect_now(peer, 0);
        return;
    }
    peer->data = reinterpret_cast<void*>((intptr_t)slot);
    g.net.lobby.slots[slot] = PlayerSlot{};
    g.net.lobby.slots[slot].used = true;

    WelcomeMsg wm{ MsgType::Welcome, slot };
    ENetPacket* pkt = enet_packet_create(&wm, sizeof(wm), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, CH_RELIABLE, pkt);

    broadcast_lobby(g);
}

void handle_host_disconnect(Game& g, ENetPeer* peer) {
    int slot = slot_of(peer);
    if (slot > 0 && slot < MAX_PLAYERS) g.net.lobby.slots[slot] = PlayerSlot{};
    broadcast_lobby(g);
}

void handle_host_receive(Game& g, ENetPeer* peer, ENetPacket* pkt) {
    if (pkt->dataLength < 1) return;
    int slot = slot_of(peer);
    if (slot <= 0 || slot >= MAX_PLAYERS) return; // no assigned slot (shouldn't happen)
    MsgType type = (MsgType)pkt->data[0];

    if (type == MsgType::Hello && pkt->dataLength >= sizeof(HelloMsg)) {
        HelloMsg hm; std::memcpy(&hm, pkt->data, sizeof(hm));
        PlayerSlot& sl = g.net.lobby.slots[slot];
        sl.skin  = hm.skin;
        sl.ready = hm.ready != 0;
        std::memcpy(sl.name, hm.name, sizeof(sl.name));
        broadcast_lobby(g);
    } else if (type == MsgType::Input && pkt->dataLength >= sizeof(InputMsg)) {
        InputMsg im; std::memcpy(&im, pkt->data, sizeof(im));
        g.inputs[slot] = im.input;
    }
}

void decode_snapshot(const enet_uint8* data, size_t len) {
    if (len < sizeof(SnapHeader)) return;
    SnapHeader hdr; std::memcpy(&hdr, data, sizeof(hdr));
    const enet_uint8* p = data + sizeof(hdr);

    size_t need = sizeof(SnapHeader)
        + (size_t)hdr.nPlayers    * sizeof(PlayerWire)
        + (size_t)hdr.nEnemies    * sizeof(EnemyWire)
        + (size_t)hdr.nItems      * sizeof(ItemWire)
        + (size_t)hdr.nPlatforms  * sizeof(PlatformWire)
        + (size_t)hdr.nProjectiles* sizeof(ProjectileWire);
    if (len < need) return; // truncated/corrupt — drop this packet

    auto take = [&](auto& vec, uint16_t n) {
        vec.resize(n);
        size_t bytes = (size_t)n * sizeof(typename std::decay_t<decltype(vec)>::value_type);
        if (bytes) std::memcpy(vec.data(), p, bytes);
        p += bytes;
    };
    gLatest.genCol = hdr.genCol;
    take(gLatest.players,     hdr.nPlayers);
    take(gLatest.enemies,     hdr.nEnemies);
    take(gLatest.items,       hdr.nItems);
    take(gLatest.platforms,   hdr.nPlatforms);
    take(gLatest.projectiles, hdr.nProjectiles);
    gLatest.valid = true;
}

void handle_client_receive(Game& g, ENetPacket* pkt) {
    if (pkt->dataLength < 1) return;
    MsgType type = (MsgType)pkt->data[0];

    switch (type) {
        case MsgType::Welcome: {
            if (pkt->dataLength < sizeof(WelcomeMsg)) break;
            WelcomeMsg wm; std::memcpy(&wm, pkt->data, sizeof(wm));
            g.net.lobby.localSlot = wm.slot;
            g.net.connected = true;
            break;
        }
        case MsgType::LobbyState: {
            if (pkt->dataLength < sizeof(LobbyStateMsg)) break;
            LobbyStateMsg lm; std::memcpy(&lm, pkt->data, sizeof(lm));
            int keepLocal = g.net.lobby.localSlot; // PRESERVE — this message doesn't know "us"
            for (int s = 0; s < MAX_PLAYERS; ++s) {
                g.net.lobby.slots[s].used  = lm.slots[s].used  != 0;
                g.net.lobby.slots[s].ready = lm.slots[s].ready != 0;
                g.net.lobby.slots[s].skin  = lm.slots[s].skin;
                std::memcpy(g.net.lobby.slots[s].name, lm.slots[s].name, sizeof(g.net.lobby.slots[s].name));
            }
            g.net.lobby.seed      = lm.seed;
            g.net.lobby.started   = lm.started != 0;
            g.net.lobby.localSlot = keepLocal;
            break;
        }
        case MsgType::StartGame: {
            if (pkt->dataLength < sizeof(StartGameMsg)) break;
            StartGameMsg sm; std::memcpy(&sm, pkt->data, sizeof(sm));
            g.net.lobby.seed = sm.seed;
            game::start_game_from_lobby(g); // sets g.mode = Playing
            break;
        }
        case MsgType::Snapshot:
            decode_snapshot(pkt->data, pkt->dataLength);
            break;
        default: break;
    }
}

} // namespace

bool start_host(uint16_t port, int maxClients) {
    if (!ensure_enet_init()) return false;
    close_host(); // re-hosting: drop any previous session, but leave a client side (if any) alone
    ENetAddress addr{}; addr.host = ENET_HOST_ANY; addr.port = port;
    gHost = enet_host_create(&addr, (size_t)maxClients, CHANNEL_COUNT, 0, 0);
    return gHost != nullptr;
}

bool start_client(const char* ip, uint16_t port) {
    if (!ensure_enet_init()) return false;
    close_client(); // re-joining: drop any previous session, but leave a host side (if any) alone
    gClient = enet_host_create(nullptr, 1, CHANNEL_COUNT, 0, 0);
    if (!gClient) return false;
    ENetAddress addr{}; addr.port = port;
    if (enet_address_set_host_ip(&addr, ip) != 0) {
        enet_host_destroy(gClient); gClient = nullptr;
        return false;
    }
    gServerPeer = enet_host_connect(gClient, &addr, CHANNEL_COUNT, 0);
    if (!gServerPeer) { enet_host_destroy(gClient); gClient = nullptr; return false; }
    return true; // actual connection completes asynchronously, seen in poll()
}

void shutdown() {
    close_transport();
    if (gEnetInited) { enet_deinitialize(); gEnetInited = false; }
}

bool is_active() { return gHost != nullptr || gClient != nullptr; }

int peer_count() { return gHost ? (int)gHost->connectedPeers : 0; }

void poll(Game& g) {
    ENetHost* active = (g.net.role == NetRole::Host) ? gHost : gClient;
    if (!active) return;
    ENetEvent ev;
    while (enet_host_service(active, &ev, 0) > 0) {
        fprintf(stderr, "DEBUG poll role=%d evtype=%d\n", (int)g.net.role, (int)ev.type);
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                if (g.net.role == NetRole::Host) handle_host_connect(g, ev.peer);
                else                              send_hello(g);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                if (g.net.role == NetRole::Host) handle_host_disconnect(g, ev.peer);
                else                              g.net.connected = false;
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                if (g.net.role == NetRole::Host) handle_host_receive(g, ev.peer, ev.packet);
                else                              handle_client_receive(g, ev.packet);
                enet_packet_destroy(ev.packet);
                break;
            default: break;
        }
    }
}

void send_hello(Game& g) {
    if (!gServerPeer) return;
    const PlayerSlot& sl = g.net.lobby.slots[g.net.lobby.localSlot];
    HelloMsg hm{};
    hm.type  = MsgType::Hello;
    hm.skin  = sl.skin;
    hm.ready = sl.ready ? 1 : 0;
    std::memcpy(hm.name, sl.name, sizeof(hm.name));
    ENetPacket* pkt = enet_packet_create(&hm, sizeof(hm), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(gServerPeer, CH_RELIABLE, pkt);
}

void broadcast_lobby(Game& g) {
    if (!gHost) return;
    LobbyStateMsg lm{};
    lm.type = MsgType::LobbyState;
    for (int s = 0; s < MAX_PLAYERS; ++s) {
        lm.slots[s].used  = g.net.lobby.slots[s].used  ? 1 : 0;
        lm.slots[s].ready = g.net.lobby.slots[s].ready ? 1 : 0;
        lm.slots[s].skin  = g.net.lobby.slots[s].skin;
        std::memcpy(lm.slots[s].name, g.net.lobby.slots[s].name, sizeof(lm.slots[s].name));
    }
    lm.seed    = g.net.lobby.seed;
    lm.started = g.net.lobby.started ? 1 : 0;
    ENetPacket* pkt = enet_packet_create(&lm, sizeof(lm), ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast(gHost, CH_RELIABLE, pkt);
}

void send_start(Game& g) {
    if (!gHost) return;
    StartGameMsg sm{ MsgType::StartGame, g.net.lobby.seed };
    ENetPacket* pkt = enet_packet_create(&sm, sizeof(sm), ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast(gHost, CH_RELIABLE, pkt);
}

void send_input(Game& g) {
    if (!gServerPeer) return;
    InputMsg im{ MsgType::Input, g.inputs[g.net.lobby.localSlot] };
    // Reliable + in-order per channel == "reliable-sequenced": jumpSeq edges must never be lost.
    ENetPacket* pkt = enet_packet_create(&im, sizeof(im), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(gServerPeer, CH_RELIABLE, pkt);
}

void broadcast_snapshot(Game& g) {
    if (!gHost || g.net.role != NetRole::Host) return;
    // ponytail: -1e9 so the very first call always sends immediately (GetTime() reads ~0 right
    // after InitWindow, and reads a stuck 0 with no raylib context at all — e.g. headless
    // tests — either way "0 - 0 < 1/30" would otherwise throttle the first snapshot forever).
    static double lastSend = -1e9;
    double now = GetTime();
    if (now - lastSend < 1.0 / SNAPSHOT_HZ) return;
    lastSend = now;

    World& w = g.world;
    SnapHeader hdr{};
    hdr.type         = MsgType::Snapshot;
    hdr.genCol       = w.genCol;
    hdr.nPlayers     = (uint16_t)w.players.size();
    hdr.nEnemies     = (uint16_t)w.enemies.size();
    hdr.nItems       = (uint16_t)w.items.size();
    hdr.nPlatforms   = (uint16_t)w.platforms.size();
    hdr.nProjectiles = (uint16_t)w.projectiles.size();

    std::vector<enet_uint8> buf;
    buf.reserve(sizeof(hdr)
        + hdr.nPlayers * sizeof(PlayerWire) + hdr.nEnemies * sizeof(EnemyWire)
        + hdr.nItems * sizeof(ItemWire) + hdr.nPlatforms * sizeof(PlatformWire)
        + hdr.nProjectiles * sizeof(ProjectileWire));
    auto append = [&](const void* d, size_t n) {
        const enet_uint8* b = (const enet_uint8*)d;
        buf.insert(buf.end(), b, b + n);
    };
    append(&hdr, sizeof(hdr));
    for (const auto& p : w.players) {
        PlayerWire pw{};
        pw.id = p.id; pw.px = p.pos.x; pw.py = p.pos.y; pw.vx = p.vel.x; pw.vy = p.vel.y;
        pw.hp = p.hp; pw.maxHp = p.maxHp;
        pw.facing = (p.facing == Facing::Right) ? 1 : 0;
        pw.weapon = (uint8_t)p.weapon;
        pw.invuln = p.invuln; pw.alive = p.alive; pw.invincible = p.invincible;
        pw.distance = p.distance;
        append(&pw, sizeof(pw));
    }
    for (const auto& e : w.enemies) {
        EnemyWire ew{};
        ew.px = e.pos.x; ew.py = e.pos.y; ew.vx = e.vel.x; ew.vy = e.vel.y;
        ew.hp = e.hp; ew.state = (uint8_t)e.state;
        ew.facing = (e.facing == Facing::Right) ? 1 : 0;
        ew.alive = e.alive;
        append(&ew, sizeof(ew));
    }
    for (const auto& it : w.items) {
        ItemWire iw{}; iw.alive = it.alive; append(&iw, sizeof(iw));
    }
    for (const auto& pf : w.platforms) {
        PlatformWire pfw{};
        pfw.rx = pf.rect.x; pfw.ry = pf.rect.y;
        pfw.active = pf.active; pfw.triggered = pf.triggered;
        append(&pfw, sizeof(pfw));
    }
    for (const auto& pr : w.projectiles) {
        ProjectileWire prw{};
        prw.px = pr.pos.x; prw.py = pr.pos.y; prw.vx = pr.vel.x; prw.vy = pr.vel.y;
        prw.fromPlayer = pr.fromPlayer;
        append(&prw, sizeof(prw));
    }

    ENetPacket* pkt = enet_packet_create(buf.data(), buf.size(), 0); // unreliable (sequenced by default)
    enet_host_broadcast(gHost, CH_SNAPSHOT, pkt);
}

void sync_client(Game& g, float dt) {
    if (g.net.role != NetRole::Client || !gLatest.valid) return;
    World& w = g.world;

    while (w.genCol < gLatest.genCol) worldgen::ensure(w, (float)gLatest.genCol * TILE);

    // DESYNC GUARD: enemies/items/platforms are append-only & index-aligned to generation.
    // If our local world hasn't generated as many as the host has, generation diverged
    // (shouldn't happen — same seed, same deterministic worldgen) — skip this snapshot rather
    // than index out of range or silently misapply state to the wrong entity.
    if (w.enemies.size()   < gLatest.enemies.size()   ||
        w.items.size()     < gLatest.items.size()     ||
        w.platforms.size() < gLatest.platforms.size()) {
        TraceLog(LOG_WARNING,
            "net: client world smaller than host snapshot (enemies %zu/%zu items %zu/%zu "
            "platforms %zu/%zu) -- skipping snapshot, worldgen may have desynced",
            w.enemies.size(), gLatest.enemies.size(), w.items.size(), gLatest.items.size(),
            w.platforms.size(), gLatest.platforms.size());
        return;
    }

    // ponytail: lazy smoothing — ease raw position toward the latest snapshot's target every
    // frame, no timestamped double-buffering. Upgrade to two-snapshot timestamped lerp if
    // motion looks stuttery on a lossy/high-latency link.
    float ease = std::fmin(1.0f, SNAP_EASE_RATE * dt);

    // players: full list (not generated), applied by index, matched back to the local player
    // via id since it's the one stable identity we have.
    if (w.players.size() != gLatest.players.size()) w.players.resize(gLatest.players.size());
    for (size_t i = 0; i < gLatest.players.size(); ++i) {
        const PlayerWire& pw = gLatest.players[i];
        Player& p = w.players[i];
        p.id = pw.id;
        p.pos.x += (pw.px - p.pos.x) * ease;
        p.pos.y += (pw.py - p.pos.y) * ease;
        p.vel = { pw.vx, pw.vy };
        p.hp = pw.hp; p.maxHp = pw.maxHp;
        p.facing = pw.facing ? Facing::Right : Facing::Left;
        p.weapon = (Weapon)pw.weapon;
        p.invuln = pw.invuln;
        p.alive = pw.alive != 0;
        p.invincible = pw.invincible != 0;
        p.distance = pw.distance;
    }
    for (size_t i = 0; i < w.players.size(); ++i) // keep localId valid after any reorder/resize
        if (w.players[i].id == g.net.lobby.localSlot) w.localId = (int)i;

    // enemies/items/platforms: index-aligned to generation, update the first N in place.
    for (size_t i = 0; i < gLatest.enemies.size(); ++i) {
        const EnemyWire& ew = gLatest.enemies[i];
        Enemy& e = w.enemies[i];
        e.pos.x += (ew.px - e.pos.x) * ease;
        e.pos.y += (ew.py - e.pos.y) * ease;
        e.vel = { ew.vx, ew.vy };
        e.hp = ew.hp;
        e.state = (EnemyState)ew.state;
        e.facing = ew.facing ? Facing::Right : Facing::Left;
        e.alive = ew.alive != 0;
    }
    for (size_t i = 0; i < gLatest.items.size(); ++i)
        w.items[i].alive = gLatest.items[i].alive != 0;
    for (size_t i = 0; i < gLatest.platforms.size(); ++i) {
        const PlatformWire& pfw = gLatest.platforms[i];
        Platform& pf = w.platforms[i];
        pf.rect.x += (pfw.rx - pf.rect.x) * ease;
        pf.rect.y += (pfw.ry - pf.rect.y) * ease;
        pf.active = pfw.active != 0;
        pf.triggered = pfw.triggered != 0;
    }

    // projectiles: full clear+refill every snapshot (no stable identity across snapshots since
    // they're not index-aligned to generation). We still ease using whatever was at the same
    // slot last frame as a rough previous-position proxy — cheap and looks fine since
    // projectiles move ~monotonically; a projectile dying the same frame a new one spawns into
    // its slot causes a one-frame visual pop.
    // ponytail: give projectiles a host-assigned stable id if that pop becomes noticeable.
    std::vector<Projectile> newProj(gLatest.projectiles.size());
    for (size_t i = 0; i < gLatest.projectiles.size(); ++i) {
        const ProjectileWire& prw = gLatest.projectiles[i];
        Vector2 start = (i < w.projectiles.size()) ? w.projectiles[i].pos : Vector2{ prw.px, prw.py };
        newProj[i].pos = { start.x + (prw.px - start.x) * ease, start.y + (prw.py - start.y) * ease };
        newProj[i].vel = { prw.vx, prw.vy };
        newProj[i].fromPlayer = prw.fromPlayer != 0;
        newProj[i].alive = true;
    }
    w.projectiles = std::move(newProj);
}

} // namespace game::net
