@echo off
rem ===========================================================================
rem  BEYOND: TWO SOULS VR MOD - UNINSTALLER
rem
rem  Removes the mod from the game folder, then the mod's own files from this B2S_VR_MOD
rem  folder (the folder too once it is empty).
rem  Your saves and the game's own files are never touched.
rem
rem  The work is done by tools\b2svr_setup.ps1 - read it to see every step.
rem ===========================================================================
setlocal
rem  run from the game folder: a folder that is some window's current folder cannot be deleted
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\b2svr_setup.ps1" -Mode Uninstall
exit /b 0
