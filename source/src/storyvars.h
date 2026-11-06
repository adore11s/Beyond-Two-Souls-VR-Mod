#pragma once
// Story variables for crash reports (1.0.0, owner 2026-11-06: "make the crash show what level they were in and what
// variables of story they had"). Read-only: nothing here creates or changes a variable or touches a save.
//
// STATIC / SOURCE (2026-11-06):
// - Scripts read a variable by name through FUN_140b05c30(entity, name) - its only callers are the script handlers
//   Get / Set Variable Bool / Int / Float / CRC32. GetVariableInt (FUN_140b092d0) / GetVariableBool (FUN_140b09600)
//   are told apart by the code at the lookup's return address (140b094d3 / 140b09803, see ReaderAt). A miss reads as
//   7777 (Int) / false (Bool).
// - The entity's VARIABLE_CONTAINER is entity+0x30 (ctor FUN_140b28040, 0xa8 bytes): its VARIABLE nodes are a list at
//   +0x60 (FUN_140b27d40 walks it; next +0x10, name CRC +0x38), spin lock +0x80 (owner thread) / +0x84 (depth).
//   VARIABLE (FUN_140b27850, 0x58 bytes, vt QDT::KCOM::VARIABLE): +0x30 value holder, +0x38 CRC, +0x40 name text,
//   +0x49 type (0 bool, 1 int, 6 object reference, FUN_140b7c9f0), value at holder+0.
// - The scene scripts (tools/lua51_calls.py "SetVariable(Bool|Int)") keep the player's story decisions as int variables
//   "Local.DT<chapter><decision>" (189 names, e.g. Local.DT0801KissMatt) on QDR.Infraworld_Var.PS4UserSaveData.
// The lookup hook only records what the game read; the crash report then lists the variables of the object that holds
// the Local.DT* decisions (walked under SEH, no lock taken) and the story variables the game asked for and did not find.
#include <cstdint>

namespace b2s::storyvars {

// Once, at startup: the observer on the variable lookup.
void Install();
bool Available();

// 1 = the caller is GetVariableInt, 2 = GetVariableBool, 0 = any other (setters, floats, CRC32 strings).
int ReaderAt(const void* returnAddress);

// Developer build only (devtools): called after every lookup with what the game found (may return another variable).
using AfterFn = uint8_t* (*)(void* entity, void* name, uint8_t* found, int reader);
void SetAfter(AfterFn fn);

// Crash report: one call per line.
void DescribeForCrash(void (*add)(const char* line, void* ctx), void* ctx);

}  // namespace b2s::storyvars
