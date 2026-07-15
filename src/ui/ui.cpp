#include "core/api.hpp"
#include "menu_util.hpp"
#include "screen.hpp" // virtual_mouse(): clicks must map through the letterbox transform

// Screen-space menus/HUD. May read input (raylib), unlike render::draw_world. Primitives +
// DrawText (default font) only — no font/asset files exist.
namespace game::ui {

namespace {

constexpr int MENU_COUNT = 4;
const char* const MENU_LABELS[MENU_COUNT] = { "Jouer", "Multijoueur", "Reglages", "Quitter" };
constexpr int MENU_TOP = 260, MENU_STEP = 50;

int menu_nav_delta() {
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) return 1;
    if (IsKeyPressed(KEY_UP)   || IsKeyPressed(KEY_W)) return -1;
    return 0;
}

const char* weapon_name(Weapon w) {
    switch (w) {
        case Weapon::Pistol: return "Pistol";
        case Weapon::Rapid:  return "Rapid";
        case Weapon::Double: return "Double";
    }
    return "?";
}

// ---- multiplayer lobby sub-screens ----
// ponytail: a local (non-Game) view state — types.hpp is read-only and this is a pure UI
// concern (which lobby screen we're on), not shared/network state.
enum class LobbyView { Chooser, JoinInput, Room };
LobbyView lobbyView = LobbyView::Chooser;
int       chooserSel = 0;
char      lobbyErr[80] = {};

constexpr int CHOOSER_COUNT = 2;
const char* const CHOOSER_LABELS[CHOOSER_COUNT] = { "Heberger", "Rejoindre" };

// true when at least one slot is used and every used slot is ready (host launch gate).
bool all_ready(const Lobby& lob) {
    bool any = false;
    for (int s = 0; s < MAX_PLAYERS; ++s) {
        if (!lob.slots[s].used) continue;
        any = true;
        if (!lob.slots[s].ready) return false;
    }
    return any;
}

void draw_chooser(Game& g) {
    DrawText("MULTIJOUEUR (LAN)", 100, 120, 48, RAYWHITE);
    chooserSel = wrap_index(chooserSel, menu_nav_delta(), CHOOSER_COUNT);
    for (int i = 0; i < CHOOSER_COUNT; ++i) {
        Color c = (i == chooserSel) ? YELLOW : GRAY;
        DrawText(CHOOSER_LABELS[i], 120, 260 + i * MENU_STEP, 32, c);
        if (i == chooserSel) DrawText(">", 90, 260 + i * MENU_STEP, 32, YELLOW);
    }
    if (lobbyErr[0]) DrawText(lobbyErr, 120, 400, 22, RED);
    DrawText("ESC : retour au menu", 120, 460, 20, GRAY);

    if (!(IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))) return;
    lobbyErr[0] = 0;
    if (chooserSel == 0) { // Heberger
        if (net::start_host((uint16_t)g.net.port, MAX_PLAYERS)) {
            g.net.role      = NetRole::Host;
            g.net.connected = true; // session is open now; start_host takes no Game& to set this itself
            g.net.lobby     = Lobby{};
            g.net.lobby.localSlot = 0;
            g.net.lobby.slots[0].used = true;
            g.net.lobby.slots[0].skin = 0;
            g.net.lobby.seed = random_seed(); // fresh procedural map each host session
            lobbyView = LobbyView::Room;
        } else {
            TextCopy(lobbyErr, "Echec: impossible d'heberger (port deja utilise ?)");
        }
    } else { // Rejoindre
        lobbyView = LobbyView::JoinInput;
    }
}

void draw_join_input(Game& g) {
    DrawText("REJOINDRE - IP de l'hote", 100, 120, 40, RAYWHITE);

    for (int ch = GetCharPressed(); ch > 0; ch = GetCharPressed())
        ip_edit(g.net.joinIp, (int)sizeof(g.net.joinIp), ch, false);
    if (IsKeyPressed(KEY_BACKSPACE)) ip_edit(g.net.joinIp, (int)sizeof(g.net.joinIp), 0, true);

    DrawText(TextFormat("IP: %s", g.net.joinIp), 120, 220, 32, YELLOW);
    DrawText("Chiffres et points uniquement, ex: 192.168.1.10", 120, 256, 18, DARKGRAY);
    DrawText(TextFormat("Port: %d", g.net.port), 120, 290, 24, GRAY);
    if (lobbyErr[0]) DrawText(lobbyErr, 120, 340, 22, RED);
    DrawText("ENTREE : rejoindre   ESC : retour", 120, 400, 22, GRAY);

    if (!(IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))) return;
    lobbyErr[0] = 0;
    if (net::start_client(g.net.joinIp, (uint16_t)g.net.port)) {
        g.net.role      = NetRole::Client;
        g.net.connected = false; // async: net::poll flips this on Welcome
        g.net.lobby     = Lobby{};
        lobbyView = LobbyView::Room;
    } else {
        TextCopy(lobbyErr, "Echec de connexion");
    }
}

void draw_room(Game& g) {
    NetState& ns  = g.net; // NB: named away from the `net` namespace (net::broadcast_lobby etc.)
    Lobby&    lob = ns.lobby;
    DrawText(ns.role == NetRole::Host ? "SALON (HOTE)" : "SALON (CLIENT)", 100, 80, 36, RAYWHITE);

    if (!ns.connected) {
        DrawText("Connexion en cours...", 120, 200, 28, YELLOW);
        DrawText("ESC : annuler", 120, SCREEN_H - 60, 20, GRAY);
        return;
    }

    int y = 160;
    for (int s = 0; s < MAX_PLAYERS; ++s) {
        const PlayerSlot& slot = lob.slots[s];
        bool isLocal = (s == lob.localSlot);
        if (!slot.used) {
            DrawText(TextFormat("%d. -- libre --", s + 1), 120, y, 24, Color{ 80, 80, 80, 255 });
        } else {
            DrawRectangle(120, y + 2, 20, 20, skin_color(slot.skin));
            DrawRectangleLines(120, y + 2, 20, 20, BLACK);
            const char* name = slot.name[0] ? slot.name : "(sans nom)";
            DrawText(TextFormat("%d. %s  %s%s", s + 1, name, slot.ready ? "PRET" : "...",
                                 isLocal ? "  (vous)" : ""),
                     150, y, 24, isLocal ? YELLOW : GRAY);
        }
        y += 32;
    }

    bool changed = false;
    PlayerSlot& mine = lob.slots[lob.localSlot];
    if (IsKeyPressed(KEY_LEFT))  { mine.skin = wrap_index(mine.skin, -1, 4); changed = true; }
    if (IsKeyPressed(KEY_RIGHT)) { mine.skin = wrap_index(mine.skin,  1, 4); changed = true; }
    if (IsKeyPressed(KEY_SPACE)) { mine.ready = !mine.ready; changed = true; }
    if (changed) {
        if (ns.role == NetRole::Host) net::broadcast_lobby(g);
        else                          net::send_hello(g);
    }

    DrawText("Gauche/Droite : skin   Espace : pret", 120, y + 10, 20, DARKGRAY);

    if (ns.role == NetRole::Host) {
        bool ready = all_ready(lob);
        DrawText("ENTREE : lancer la partie", 120, y + 44, 22, ready ? GREEN : Color{ 90, 90, 90, 255 });
        if (ready && IsKeyPressed(KEY_ENTER)) {
            net::send_start(g);
            start_game_from_lobby(g);
        }
    }
    DrawText("ESC : quitter le salon", 120, SCREEN_H - 60, 20, GRAY);
}

} // namespace

void menu(Game& g) {
    // ponytail: selection index lives as a static local, not a Game field — types.hpp is
    // read-only and this state is purely a UI concern. See contract_notes.
    static int sel = 0;
    sel = wrap_index(sel, menu_nav_delta(), MENU_COUNT);

    DrawText("PLATFORMER", 100, 120, 60, RAYWHITE);
    for (int i = 0; i < MENU_COUNT; ++i) {
        Color c = (i == sel) ? YELLOW : GRAY;
        DrawText(MENU_LABELS[i], 120, MENU_TOP + i * MENU_STEP, 32, c);
        if (i == sel) DrawText(">", 90, MENU_TOP + i * MENU_STEP, 32, YELLOW);
    }
    bool chosen = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { // optional mouse: click a label to pick it
        Vector2 m = virtual_mouse();
        for (int i = 0; i < MENU_COUNT; ++i) {
            Rectangle r{ 90, (float)(MENU_TOP + i * MENU_STEP) - 4, 320, 40 };
            if (CheckCollisionPointRec(m, r)) { sel = i; chosen = true; break; }
        }
    }
    if (!chosen) return;
    switch (sel) {
        case 0: new_game(g, 0); break; // seed 0 -> new_game rolls a random seed (fresh map each run)
        case 1: // "Multijoueur": always enter fresh at the host/join chooser
            lobbyView  = LobbyView::Chooser;
            chooserSel = 0;
            lobbyErr[0] = 0;
            g.mode = Mode::Lobby;
            break;
        case 2: g.mode = Mode::Settings; break;
        case 3: g.wantQuit = true; break; // main's loop exits on wantQuit (ESC no longer quits)
    }
}

void settings(Game& g) {
    DrawText("REGLAGES", 100, 120, 48, RAYWHITE);

    if (IsKeyPressed(KEY_LEFT)  || IsKeyPressed(KEY_A)) g.settings.volume -= 0.1f;
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) g.settings.volume += 0.1f;
    if (g.settings.volume < 0.0f) g.settings.volume = 0.0f;
    if (g.settings.volume > 1.0f) g.settings.volume = 1.0f;
    DrawText(TextFormat("Volume: %.0f%%  (Gauche/Droite)", g.settings.volume * 100.0f),
             120, 240, 28, RAYWHITE);

    if (IsKeyPressed(KEY_ENTER)) {
        ToggleFullscreen();
        g.settings.fullscreen = !g.settings.fullscreen;
    }
    DrawText(TextFormat("Plein ecran: %s  (Entree)", g.settings.fullscreen ? "ON" : "OFF"),
             120, 290, 28, RAYWHITE);

    // ponytail: rebinding later — keys stay hardcoded (main.cpp) until remap UI is asked for.
    DrawText("Touches : fixes pour l'instant", 120, 340, 24, DARKGRAY);

    DrawText("ESC : retour", 120, 420, 24, GRAY);
    if (IsKeyPressed(KEY_ESCAPE)) g.mode = Mode::Menu;
}

void lobby(Game& g) {
    // ESC steps back ONE level of the lobby flow instead of always dropping to the menu.
    if (IsKeyPressed(KEY_ESCAPE)) {
        switch (lobbyView) {
            case LobbyView::Room: // leave the session and go all the way back to the menu
                net::shutdown();
                g.net = NetState{};
                lobbyView = LobbyView::Chooser;
                g.mode = Mode::Menu;
                break;
            case LobbyView::JoinInput: // no session yet: just back out of the IP entry
                lobbyView = LobbyView::Chooser;
                break;
            case LobbyView::Chooser:
                g.mode = Mode::Menu;
                break;
        }
        lobbyErr[0] = 0;
        return;
    }
    switch (lobbyView) {
        case LobbyView::Chooser:   draw_chooser(g);    break;
        case LobbyView::JoinInput: draw_join_input(g); break;
        case LobbyView::Room:      draw_room(g);       break;
    }
}

void hud(const World& w) {
    if (w.players.empty()) return;
    const Player& p = w.players[w.localId];

    // ponytail: these mirror core/game.cpp's mobility knobs (not exported via api.hpp) — HUD-only,
    // keep in sync if the gameplay values change. Only used to draw the readiness meters.
    constexpr float DASH_CD_HUD    = 0.7f; // == DASH_CD in game.cpp
    constexpr int   MAX_AIR_JUMPS  = 1;    // == MAX_AIR_JUMPS in game.cpp

    // ---- hearts ----
    for (int i = 0; i < p.maxHp; ++i) {
        Rectangle r{ (float)(20 + i * 34), 20, 28, 28 };
        DrawRectangleRec(r, (i < p.hp) ? RED : Color{ 70, 70, 70, 255 });
        DrawRectangleLinesEx(r, 1, BLACK);
    }
    DrawText(TextFormat("Arme: %s", weapon_name(p.weapon)), 20, 56, 22, RAYWHITE);
    DrawText(TextFormat("Distance: %.0f", p.distance), 20, 84, 22, RAYWHITE);

    // ---- dash cooldown meter (full + cyan = ready) ----
    float dashReady = (p.dashCd <= 0.f) ? 1.0f : 1.0f - p.dashCd / DASH_CD_HUD;
    if (dashReady < 0.f) dashReady = 0.f;
    bool dashUp = dashReady >= 1.0f;
    Rectangle dbg{ 20, 116, 120, 14 };
    DrawRectangleRec(dbg, Color{ 45, 45, 55, 255 });
    DrawRectangle(20, 116, (int)(120 * dashReady), 14, dashUp ? Color{ 90, 200, 255, 255 } : Color{ 70, 120, 150, 255 });
    DrawRectangleLinesEx(dbg, 1, BLACK);
    DrawText("DASH", 146, 116, 14, dashUp ? SKYBLUE : GRAY);

    // ---- double-jump pip (lit = a mid-air jump is available) ----
    bool djReady = p.airJumps < MAX_AIR_JUMPS;
    DrawCircleV({ 28, 145 }, 7, djReady ? Color{ 90, 220, 120, 255 } : Color{ 60, 60, 60, 255 });
    DrawCircleLines(28, 145, 7, BLACK);
    DrawText("2x SAUT", 42, 138, 14, djReady ? GREEN : GRAY);

    if (p.invincible)
        DrawText(TextFormat("INVINCIBLE %.1fs", p.powerTimer), 20, 164, 22, GOLD);

    if (w.players.size() > 1) { // multiplayer: skin marker per teammate (score removed with coins)
        int x = 20, y = 190;
        for (size_t i = 0; i < w.players.size(); ++i) {
            if (i == (size_t)w.localId) DrawRectangleLines(x - 2, y - 2, 24, 24, WHITE);
            DrawRectangle(x, y, 20, 20, w.players[i].color);
            x += 30;
        }
    }
}

void pause(Game& g) {
    // ponytail: reuses menu()'s static-sel + nav-delta + mouse-click pattern. ESC is NOT read
    // here — main.cpp owns the Playing<->Paused toggle and flips modes itself on ESC.
    static int sel = 0;
    constexpr int PAUSE_COUNT = 2;
    const char* const PAUSE_LABELS[PAUSE_COUNT] = { "Reprendre", "Quitter vers le menu" };
    constexpr int PAUSE_TOP = 320;
    sel = wrap_index(sel, menu_nav_delta(), PAUSE_COUNT);

    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Color{ 0, 0, 0, 160 }); // dim the frozen game behind
    DrawText("PAUSE", 100, 180, 56, RAYWHITE);
    for (int i = 0; i < PAUSE_COUNT; ++i) {
        Color c = (i == sel) ? YELLOW : GRAY;
        DrawText(PAUSE_LABELS[i], 120, PAUSE_TOP + i * MENU_STEP, 32, c);
        if (i == sel) DrawText(">", 90, PAUSE_TOP + i * MENU_STEP, 32, YELLOW);
    }

    bool chosen = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { // optional mouse: click a label to pick it
        Vector2 m = virtual_mouse();
        for (int i = 0; i < PAUSE_COUNT; ++i) {
            Rectangle r{ 90, (float)(PAUSE_TOP + i * MENU_STEP) - 4, 380, 40 };
            if (CheckCollisionPointRec(m, r)) { sel = i; chosen = true; break; }
        }
    }
    if (!chosen) return;
    switch (sel) {
        case 0: g.mode = Mode::Playing; break;
        case 1: // "Quitter vers le menu"
            if (g.net.role != NetRole::Solo) {
                net::shutdown();
                g.net = NetState{};
            }
            g.mode = Mode::Menu;
            break;
    }
}

void gameover(Game& g) {
    float dist = g.world.players.empty() ? 0.0f : g.world.players[g.world.localId].distance;
    DrawText("GAME OVER", 420, 260, 60, RED);
    DrawText(TextFormat("Distance: %.0f", dist), 470, 340, 28, RAYWHITE);
    DrawText("ENTREE/ESC : Menu", 470, 380, 24, GRAY);
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
        if (g.net.role != NetRole::Solo) {
            net::shutdown();
            g.net = NetState{};
        }
        g.mode = Mode::Menu;
    }
}

} // namespace game::ui
