@echo off
cd /d "C:\Users\Yang\.openclaw\workspace\projects\ProcessNetMonitor\plugin"

:: Setup MSVC environment
set MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
set SDK=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0

set INCLUDE=%MSVC%\VC\Tools\MSVC\14.44.35207\include;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared
set LIB=%MSVC%\VC\Tools\MSVC\14.44.35207\lib\x64;%SDK%\Lib\%SDKVER%\um\x64;%SDK%\Lib\%SDKVER%\ucrt\x64
set PATH=%MSVC%\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64;%PATH%

cl /nologo /O2 /EHsc /MT /std:c++17 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WINDLL /Isrc /FeProcessNetMonitor.dll src\capture.cpp src\plugin_main.cpp src\tooltip_popup.cpp /link /DLL /OUT:ProcessNetMonitor.dll iphlpapi.lib ws2_32.lib gdi32.lib user32.lib shell32.lib dwmapi.lib advapi32.lib

if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED
    pause
    exit /b 1
)

:: Deploy
taskkill /f /im TrafficMonitor.exe 2>nul
timeout /t 2 /nobreak >nul
copy /y ProcessNetMonitor.dll "C:\Users\Yang\.openclaw\workspace\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor\plugins\ProcessNetMonitor.dll"
if %ERRORLEVEL% NEQ 0 (
    echo WARNING: DLL copy failed! Close TrafficMonitor first.
    pause
    exit /b 1
)
echo DEPLOYED OK
