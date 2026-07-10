@echo off
cd /d "C:\Users\Yang\.openclaw\workspace\projects\ProcessNetMonitor\plugin"

:: Setup MSVC environment
set MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
set SDK=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0

:: Common flags
set COMMON_FLAGS=/nologo /O2 /EHsc /MT /std:c++17 /utf-8 /Zi /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WINDLL /Isrc
set COMMON_LINK=/DLL /DEBUG /MAP iphlpapi.lib ws2_32.lib gdi32.lib user32.lib shell32.lib dwmapi.lib advapi32.lib

:: Compile resource file
set "RC=%SDK%\bin\%SDKVER%\x86\rc.exe"
"%RC%" /nologo /fo src\resource.res src\resource.rc
if %ERRORLEVEL% NEQ 0 (
    echo RESOURCE COMPILE FAILED
    exit /b 1
)
set COMMON_LINK=%COMMON_LINK% src\resource.res

:: Parse architecture argument
if "%1"=="x86" goto :build_x86
if "%1"=="x64" goto :build_x64
if "%1"=="all" goto :build_all
if "%1"=="" goto :build_x64

echo Usage: build.bat [x86^|x64^|all]
echo   x86  - Build 32-bit DLL only
echo   x64  - Build 64-bit DLL only (default)
echo   all  - Build both 32-bit and 64-bit DLLs
exit /b 1

:build_x86
echo === Building 32-bit (x86) DLL ===
set INCLUDE=%MSVC%\VC\Tools\MSVC\14.44.35207\include;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared
set LIB=%MSVC%\VC\Tools\MSVC\14.44.35207\lib\x86;%SDK%\Lib\%SDKVER%\um\x86;%SDK%\Lib\%SDKVER%\ucrt\x86
set PATH=%MSVC%\VC\Tools\MSVC\14.44.35207\bin\Hostx86\x86;%PATH%

cl %COMMON_FLAGS% /FeProcessNetMonitor_x86.dll src\capture.cpp src\plugin_main.cpp src\tooltip_popup.cpp src\detail_window.cpp /link %COMMON_LINK% /OUT:ProcessNetMonitor_x86.dll
if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED (x86)
    exit /b 1
)
echo === x86 Build OK ===
goto :eof

:build_x64
echo === Building 64-bit (x64) DLL ===
set INCLUDE=%MSVC%\VC\Tools\MSVC\14.44.35207\include;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared
set LIB=%MSVC%\VC\Tools\MSVC\14.44.35207\lib\x64;%SDK%\Lib\%SDKVER%\um\x64;%SDK%\Lib\%SDKVER%\ucrt\x64
set PATH=%MSVC%\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64;%PATH%

cl %COMMON_FLAGS% /FeProcessNetMonitor.dll src\capture.cpp src\plugin_main.cpp src\tooltip_popup.cpp src\detail_window.cpp /link %COMMON_LINK% /OUT:ProcessNetMonitor.dll
if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED (x64)
    exit /b 1
)
echo === x64 Build OK ===
goto :deploy

:build_all
call :build_x86
call :build_x64
goto :deploy_all

:deploy
:: Deploy x64 DLL (use CIM to kill admin-privileged TM, works without elevation)
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"name='TrafficMonitor.exe'\" | Invoke-CimMethod -MethodName Terminate | Out-Null"
timeout /t 2 /nobreak >nul
copy /y ProcessNetMonitor.dll "C:\Users\Yang\.openclaw\workspace\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor\plugins\ProcessNetMonitor.dll"
if %ERRORLEVEL% NEQ 0 (
    echo WARNING: x64 DLL copy failed! Close TrafficMonitor first.
    exit /b 1
)
:: Start TM as user Yang (must use working directory for TM to find its config)
echo @echo off > "%TEMP%\launch_tm.bat"
echo cd /d "C:\Users\Yang\.openclaw\workspace\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor" >> "%TEMP%\launch_tm.bat"
echo start "" TrafficMonitor.exe >> "%TEMP%\launch_tm.bat"
schtasks /create /tn "LaunchTM" /tr "%TEMP%\launch_tm.bat" /sc once /st 00:00 /ru Yang /f >nul 2>&1
schtasks /run /tn "LaunchTM" >nul 2>&1
timeout /t 3 /nobreak >nul
schtasks /delete /tn "LaunchTM" /f >nul 2>&1
echo DEPLOYED OK (x64)
goto :eof

:deploy_all
:: Deploy both DLLs
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"name='TrafficMonitor.exe'\" | Invoke-CimMethod -MethodName Terminate | Out-Null"
timeout /t 2 /nobreak >nul
copy /y ProcessNetMonitor.dll "C:\Users\Yang\.openclaw\workspace\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor\plugins\ProcessNetMonitor.dll"
copy /y ProcessNetMonitor_x86.dll "C:\Users\Yang\.openclaw\workspace\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor\plugins\ProcessNetMonitor_x86.dll"
if %ERRORLEVEL% NEQ 0 (
    echo WARNING: DLL copy failed! Close TrafficMonitor first.
    exit /b 1
)
:: Start TM as user Yang
echo @echo off > "%TEMP%\launch_tm.bat"
echo cd /d "C:\Users\Yang\.openclaw\workspace\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor" >> "%TEMP%\launch_tm.bat"
echo start "" TrafficMonitor.exe >> "%TEMP%\launch_tm.bat"
schtasks /create /tn "LaunchTM" /tr "%TEMP%\launch_tm.bat" /sc once /st 00:00 /ru Yang /f >nul 2>&1
schtasks /run /tn "LaunchTM" >nul 2>&1
timeout /t 3 /nobreak >nul
schtasks /delete /tn "LaunchTM" /f >nul 2>&1
echo DEPLOYED OK (x86 + x64)
