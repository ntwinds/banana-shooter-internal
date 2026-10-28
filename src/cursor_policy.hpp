#pragma once
namespace input_policy {
struct Cursor {int lock;bool visible;};
// Respect a pause/settings menu opened while the ImGui controls were visible.
constexpr Cursor restore(Cursor previous,bool game_ui_blocked){
    return game_ui_blocked?Cursor{0,true}:previous;
}
}
