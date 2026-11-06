// Release build: no developer tools (devtools.h). Nothing here hooks the game, reads a key or writes a file.
#include "devtools.h"

#include "log.h"

namespace b2s::devtools {

bool Available() { return false; }
void Install() { Log("Developer tools: not in this build"); }
void Frame() {}
xr::FrameInput Filter(const xr::FrameInput& in) { return in; }
bool GetOverlay(Overlay*) { return false; }
uint64_t CopyPixels(uint32_t*) { return 0; }
bool Command(const char*, const char*) { return false; }

}  // namespace b2s::devtools
