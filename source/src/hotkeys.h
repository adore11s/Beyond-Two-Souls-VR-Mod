#pragma once

namespace b2s::hotkeys {

// True while the mod's keys should act: the game window or a SteamVR window (VR View, the
// SteamVR status window, its dashboard helpers) is in front. With the headset on, clicking the
// SteamVR mirror is common; a key typed into another program never toggles anything.
bool Active();

// Rising edge of a polled virtual key while Active(); one Edge object per key and caller.
struct Edge {
    bool was = false;
    bool Feed(int vk);
};

}  // namespace b2s::hotkeys
