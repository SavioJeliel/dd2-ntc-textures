@echo off
rem Only checks whether the game is compatible. Changes nothing.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -VerifyOnly %*
pause
