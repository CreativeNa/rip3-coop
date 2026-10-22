@echo off
rem Launches RIP 3 through Steam with the mods: Russian language and modern display.
rem The injector waits for the game process and loads the DLLs right after it starts.
cd /d "%~dp0build"

rem a game that is still closing would be "already running" for the injector
set tries=0
:wait_old
tasklist /fi "imagename eq rip3.exe" | find /i "rip3.exe" >nul
if errorlevel 1 goto launch
set /a tries+=1
if %tries% geq 20 (
    echo RIP 3 is still running. Close it and start this file again.
    pause
    exit /b 1
)
timeout /t 1 /nobreak >nul
goto wait_old

:launch
start "" /b cmd /c inject.exe --wait "%~dp0build\rip3video.dll" "%~dp0build\rip3ru.dll" "%~dp0build\rip3net.dll" ^> "%~dp0build\inject.log" 2^>^&1
rem give the injector a moment to start watching before Steam launches the game
timeout /t 1 /nobreak >nul
start "" "steam://rungameid/2550"
