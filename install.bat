@echo off
rem Dragon's Dogma 2 NTC texture mod installer. Asks for the game folder.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
pause
