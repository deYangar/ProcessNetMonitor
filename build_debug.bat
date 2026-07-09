@echo off
setlocal

REM ============================================================
REM Build debug version: ProcessNetMonitor_debug.dll
REM Enables DumpDebugLog output to debug.log for troubleshooting
REM ============================================================

set MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207
set SDK=C:\Program Files (x86)\Windows Kits\10
set INCLUDE=%MSVC%\include;%SDK%\Include\10.0.26100.0\um;%SDK%\Include\10.0.26100.0\ucrt;%SDK%\Include\10.0.26100.0\shared
set LIB=%MSVC%\lib\x64;%SDK%\Lib\10.0.26100.0\um\x64;%SDK%\Lib\10.0.26100.0\ucrt\x64
set PATH=%MSVC%\bin\Hostx64\x64;%PATH%

cd /d "%~dp0\plugin"

REM 1. Enable debug log in capture.h
powershell -Command "(Get-Content src\capture.h -Raw -Encoding UTF8) -replace '// void DumpDebugLog\(const std::vector<ProcTraffic>& result, double dt\);', 'void DumpDebugLog(const std::vector<ProcTraffic>& result, double dt);' -replace '// std::wstring m_debug_path;', 'std::wstring m_debug_path;' -replace '// time_t m_last_debug = 0;', 'time_t m_last_debug = 0;' | Set-Content src\capture.h -Encoding UTF8 -NoNewline"

REM 2. Enable debug log call in capture.cpp
powershell -Command "(Get-Content src\capture.cpp -Raw -Encoding UTF8) -replace '// DumpDebugLog\(result, dt\);', 'DumpDebugLog(result, dt);' | Set-Content src\capture.cpp -Encoding UTF8 -NoNewline"

REM 3. Check if DumpDebugLog body exists, if not, append from build_debug_body.txt
findstr /C:"void PacketCapture::DumpDebugLog" src\capture.cpp >nul 2>&1
if errorlevel 1 (
    echo Injecting DumpDebugLog function body...
    type build_debug_body.txt >> src\capture.cpp
)

REM 4. Compile
echo Building debug DLL (x64)...
cl /nologo /O2 /EHsc /MT /std:c++17 /utf-8 /Zi /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WINDLL /Isrc ^
   /FeProcessNetMonitor_debug.dll ^
   src\capture.cpp src\plugin_main.cpp src\tooltip_popup.cpp src\detail_window.cpp ^
   /link /DLL iphlpapi.lib ws2_32.lib gdi32.lib user32.lib shell32.lib dwmapi.lib advapi32.lib ^
   src\resource.res /OUT:ProcessNetMonitor_debug.dll

if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED
    exit /b 1
)

echo.
echo DEPLOYED: plugin\ProcessNetMonitor_debug.dll

REM 5. Revert source changes
cd /d "%~dp0"
git checkout -- plugin\src\capture.h plugin\src\capture.cpp
echo Source reverted to release state.
