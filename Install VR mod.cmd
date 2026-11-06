@echo off
rem ===========================================================================
rem  BEYOND: TWO SOULS VR MOD - INSTALL (run it once)
rem
rem  Put this whole B2S_VR_MOD folder inside your Beyond: Two Souls folder (the
rem  one containing BeyondTwoSouls_Steam.exe) and run this file. It copies the
rem  mod (dinput8.dll) next to the game's exe. After that just start the game
rem  as usual - VR starts by itself.
rem
rem  The work is done by tools\b2svr_setup.ps1 - read it to see every step.
rem ===========================================================================
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\b2svr_setup.ps1" -Mode Install & exit /b 0
