@echo off
rem Collects logs and system info into a ZIP on the Desktop, to attach when asking for help.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0report.ps1" %*
pause
