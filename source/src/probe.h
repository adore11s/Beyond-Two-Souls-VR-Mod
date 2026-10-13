#pragma once

// Read-only observation probes for RE-04 (frame boundary) and RE-05 (camera census).
// Nothing here writes engine state.
namespace b2s::probe {

void Install();

// devctl verb "frametrace [events]"; returns true when handled.
bool Command(const char* verb, const char* arg);

// ENGINE::Trace revival entry (called from the register-preserving asm thunk).
void TraceVEntry(const char* fmt, char* args);

}  // namespace b2s::probe
