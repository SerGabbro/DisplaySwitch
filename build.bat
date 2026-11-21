@echo off
rem Builds DisplaySwitch.exe with MinGW-w64 (g++ and windres in PATH).
rem   build.bat            build DisplaySwitch.exe next to this script
rem   build.bat release    also create dist\DisplaySwitch-<version>-win64.zip
setlocal
cd /d "%~dp0"

where g++ >nul 2>&1 || (echo ERROR: g++ not found in PATH. Install MinGW-w64, e.g. "winget install BrechtSanders.WinLibs.POSIX.UCRT" & exit /b 1)
where windres >nul 2>&1 || (echo ERROR: windres not found in PATH ^(it ships with MinGW-w64^). & exit /b 1)

if not exist build mkdir build

echo [1/3] Resources (icon, version info, manifest)
windres -I src src\DisplaySwitch.rc -O coff -o build\resources.o || exit /b 1

rem MinGW links its own default-manifest.o, which clashes with ours. An empty
rem object with the same name, found first through -B, takes its place.
echo [2/3] Manifest override
if not exist build\nomanifest mkdir build\nomanifest
echo.> build\nomanifest\empty.c
gcc -c build\nomanifest\empty.c -o build\nomanifest\default-manifest.o || exit /b 1

echo [3/3] Compiling
g++ -B build\nomanifest\ -std=c++17 -O2 -Wall -Wextra -static -s src\DisplaySwitch.cpp build\resources.o -o DisplaySwitch.exe -lwtsapi32 || exit /b 1

for /f "tokens=2" %%v in ('"%~dp0DisplaySwitch.exe" version') do set VERSION=%%v
if not defined VERSION (echo ERROR: the built exe does not report its version & exit /b 1)
echo Built DisplaySwitch.exe %VERSION%

if /i not "%~1"=="release" exit /b 0

echo Packaging release
if exist dist rmdir /s /q dist
mkdir dist\DisplaySwitch
copy /y DisplaySwitch.exe dist\DisplaySwitch\ >nul
copy /y profiles.example.yaml dist\DisplaySwitch\profiles.yaml >nul
copy /y README.md dist\DisplaySwitch\ >nul
copy /y LICENSE dist\DisplaySwitch\ >nul
powershell -NoProfile -Command "Compress-Archive -Path 'dist\DisplaySwitch\*' -DestinationPath 'dist\DisplaySwitch-%VERSION%-win64.zip' -Force" || exit /b 1
echo Created dist\DisplaySwitch-%VERSION%-win64.zip
