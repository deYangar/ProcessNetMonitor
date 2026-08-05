@echo off
cd /d "C:\Users\Yang\.openclaw\workspace\projects\ProcessNetMonitor\plugin"
set MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
set SDK=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0

echo === Building x64 ===
set "INCLUDE=%MSVC%\VC\Tools\MSVC\14.44.35207\include;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared"
set "LIB=%MSVC%\VC\Tools\MSVC\14.44.35207\lib\x64;%SDK%\Lib\%SDKVER%\um\x64;%SDK%\Lib\%SDKVER%\ucrt\x64"
set "PATH=%MSVC%\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64;%PATH%"
cl /nologo /O2 /EHsc /MT /std:c++17 /utf-8 /Zi /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WINDLL /Isrc /FeProcessNetMonitor.dll src\capture.cpp src\plugin_main.cpp src\tooltip_popup.cpp src\detail_window.cpp src\etw_capture.cpp /link /DLL /DEBUG /MAP iphlpapi.lib ws2_32.lib gdi32.lib user32.lib shell32.lib dwmapi.lib advapi32.lib tdh.lib src\resource.res /OUT:ProcessNetMonitor.dll
echo X64_EXIT=%ERRORLEVEL%

echo === Building x86 ===
set "INCLUDE=%MSVC%\VC\Tools\MSVC\14.44.35207\include;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared"
set "LIB=%MSVC%\VC\Tools\MSVC\14.44.35207\lib\x86;%SDK%\Lib\%SDKVER%\um\x86;%SDK%\Lib\%SDKVER%\ucrt\x86"
set "PATH=%MSVC%\VC\Tools\MSVC\14.44.35207\bin\Hostx86\x86;%PATH%"
cl /nologo /O2 /EHsc /MT /std:c++17 /utf-8 /Zi /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WINDLL /Isrc /FeProcessNetMonitor_x86.dll src\capture.cpp src\plugin_main.cpp src\tooltip_popup.cpp src\detail_window.cpp src\etw_capture.cpp /link /DLL /DEBUG /MAP iphlpapi.lib ws2_32.lib gdi32.lib user32.lib shell32.lib dwmapi.lib advapi32.lib tdh.lib src\resource.res /OUT:ProcessNetMonitor_x86.dll
echo X86_EXIT=%ERRORLEVEL%
