@echo off
rem ===========================================================================
rem  BEYOND: TWO SOULS VR MOD - UNINSTALLER
rem
rem  Moves the mod's own files (dinput8.dll, and if you want b2svr.ini and
rem  b2svr_logs) and this B2S_VR_MOD folder to the Recycle Bin, and puts back the
rem  game options the installer changed. Nothing is deleted for good, and no game
rem  file (the game's exe, your saves, ...) is ever touched.
rem
rem  The work is done by tools\b2svr_setup.ps1 - read it to see every step.
rem ===========================================================================
setlocal
rem  run from the game folder: a folder that is some window's current folder cannot be moved
cd /d "%~dp0.."
rem  one line: this file is moved to the Recycle Bin while it runs
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\b2svr_setup.ps1" -Mode Uninstall & exit /b 0
