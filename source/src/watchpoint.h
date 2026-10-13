#pragma once
// Dev probe (EXP-035): hardware data breakpoints on up to two addresses, set on every thread of the
// game, logging each distinct game-code instruction that reads or writes them (exe RVA + thread).
namespace b2s::watchpoint {

// Arms DR0/DR1 (8 bytes, read/write) for `seconds`, then disarms and logs the table.
bool Start(void* a, void* b, int seconds);

}  // namespace b2s::watchpoint
