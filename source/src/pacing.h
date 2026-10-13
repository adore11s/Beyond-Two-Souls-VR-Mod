#pragma once

// Frame pacing ownership (docs/FRAMERATE.md). The engine's own frame limiter sleeps until a
// minimum frame time has elapsed; VSync is the Present sync interval. Both are overridden
// from b2svr.ini ([b2svr] fps=, vsync=) and devctl verbs, and handed to the headset while
// OpenXR runs (xrWaitFrame paces the game).
namespace b2s::pacing {

// Reads b2svr.ini and verifies the limiter guards; the feature stays off if they mismatch.
void Install();

// Game thread, once per Present: enforces the limiter value and returns the sync interval
// the game's Present should use.
unsigned OnPresent(unsigned gameSync, bool xrRunning);

// devctl verbs "fps <n|0|game>" and "vsync <off|game>"; returns true when handled.
bool Command(const char* verb, const char* arg);

}  // namespace b2s::pacing
