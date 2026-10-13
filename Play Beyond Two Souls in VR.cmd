@echo off
rem ===========================================================================
rem  BEYOND: TWO SOULS - VR MOD
rem
rem  Put this whole B2S_VR_MOD folder inside your Beyond: Two Souls folder (the
rem  one containing BeyondTwoSouls_Steam.exe) and run this file. It installs the
rem  mod (or updates it), starts SteamVR if needed, and starts the game.
rem
rem  The work is done by tools\b2svr_setup.ps1 - read it to see every step.
rem ===========================================================================
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\b2svr_setup.ps1" -Mode Play
exit /b %errorlevel%
