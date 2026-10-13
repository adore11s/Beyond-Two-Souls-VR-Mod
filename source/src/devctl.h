#pragma once

struct IDXGISwapChain;

// Developer control channel for passive observation. It never injects game input
// (project rule: the owner plays; tooling only observes).
//
// Commands are read from <gameDir>\b2svr_cmd.txt (one per line), then the file is
// renamed to b2svr_cmd.done. Results are logged with a "DEVCTL" prefix.
//   wait <frames>              pause the queue
//   shot <name> [scale]        save the backbuffer as <name>.bmp (scale divides size, default 4)
//   lua <file>                 run a Lua file through the engine's script runner
//   log <text>                 write a marker into b2svr.log
//   stereo on|off, ipd <m>, suppress on|off, sshot <name>   (see stereo.h)
namespace b2s::devctl {

// Called once per Present on the game thread.
void OnFrame(IDXGISwapChain* sc);

}  // namespace b2s::devctl
