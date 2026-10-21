@echo off
cd /d "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\plugin"

:: Setup MSVC environment
set MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
set SDK=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0

:: Common flags
set COMMON_FLAGS=/nologo /O2 /EHsc /MT /std:c++17 /utf-8 /Zi /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WINDLL /Isrc
set COMMON_LINK=/DLL /DEBUG /MAP iphlpapi.lib ws2_32.lib gdi32.lib user32.lib shell32.lib dwmapi.lib advapi32.lib tdh.lib msimg32.lib

:: Compile resource file
set "RC=%SDK%\bin\%SDKVER%\x86\rc.exe"
"%RC%" /nologo /fo src\resource.res src\resource.rc
if %ERRORLEVEL% NEQ 0 (
    echo RESOURCE COMPILE FAILED
    exit /b 1
)
set COMMON_LINK=%COMMON_LINK% src\resource.res

:: 编译后动作：默认只编译不部署（2026-10-18 咩咩拍板方案 B——自动
:: 部署曾与手工部署流程竞争导致 TM 单实例弹框×2；要部署显式传 deploy）
set AFTER=eof

:: Parse architecture argument
if "%1"=="x86" goto :build_x86
if "%1"=="x64" goto :build_x64
if "%1"=="all" goto :build_all
if "%1"=="deploy" goto :deploy_set
if "%1"=="deploy_all" goto :deploy_all_flow
if "%1"=="" goto :build_x64

echo Usage: build.bat [x86^|x64^|all^|deploy^|deploy_all]
echo   x86 / x64 / all - build only, no deploy (default: x64)
echo   deploy          - build x64 + deploy to test slot (kill TM, copy, relaunch)
echo   deploy_all      - build both + deploy both to test slot
exit /b 1

:deploy_set
set AFTER=deploy
goto :build_x64

:deploy_all_flow
call :build_x86
if errorlevel 1 exit /b 1
call :build_x64
if errorlevel 1 exit /b 1
goto :deploy_all

:build_x86
echo === Building 32-bit (x86) DLL ===
set INCLUDE=%MSVC%\VC\Tools\MSVC\14.44.35207\include;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared
set LIB=%MSVC%\VC\Tools\MSVC\14.44.35207\lib\x86;%SDK%\Lib\%SDKVER%\um\x86;%SDK%\Lib\%SDKVER%\ucrt\x86
set PATH=%MSVC%\VC\Tools\MSVC\14.44.35207\bin\Hostx86\x86;%PATH%

cl %COMMON_FLAGS% /FeProcessNetMonitor_x86.dll src\capture.cpp src\plugin_main.cpp src\tooltip_popup.cpp src\opacity_preview.cpp src\detail_window.cpp src\etw_capture.cpp src\ip_geo.cpp src\signature.cpp src\i18n.cpp /link %COMMON_LINK% /OUT:ProcessNetMonitor_x86.dll
if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED [x86]
    exit /b 1
)
echo === x86 Build OK ===
goto :%AFTER%

:build_x64
echo === Building 64-bit (x64) DLL ===
set INCLUDE=%MSVC%\VC\Tools\MSVC\14.44.35207\include;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared
set LIB=%MSVC%\VC\Tools\MSVC\14.44.35207\lib\x64;%SDK%\Lib\%SDKVER%\um\x64;%SDK%\Lib\%SDKVER%\ucrt\x64
set PATH=%MSVC%\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64;%PATH%

cl %COMMON_FLAGS% /FeProcessNetMonitor.dll src\capture.cpp src\plugin_main.cpp src\tooltip_popup.cpp src\opacity_preview.cpp src\detail_window.cpp src\etw_capture.cpp src\ip_geo.cpp src\signature.cpp src\i18n.cpp /link %COMMON_LINK% /OUT:ProcessNetMonitor.dll
if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED [x64]
    exit /b 1
)
echo === x64 Build OK ===
goto :%AFTER%

:build_all
call :build_x86
if errorlevel 1 exit /b 1
call :build_x64
if errorlevel 1 exit /b 1
goto :%AFTER%

:deploy
:: Deploy x64 DLL (CIM kills admin-privileged TM without elevation; poll until
:: gone before copying/launching - fixed sleep races single-instance mutex)
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"name='TrafficMonitor.exe'\" | Invoke-CimMethod -MethodName Terminate | Out-Null; foreach ($i in 1..30) { Start-Sleep -Seconds 1; if (-not (Get-CimInstance Win32_Process -Filter \"name='TrafficMonitor.exe'\")) { exit 0 } }; exit 1"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: TrafficMonitor did not exit within 30s, deploy aborted
    exit /b 1
)
copy /y ProcessNetMonitor.dll "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor\plugins\ProcessNetMonitor.dll"
if %ERRORLEVEL% NEQ 0 (
    echo WARNING: x64 DLL copy failed! Close TrafficMonitor first.
    exit /b 1
)
:: Deploy language files to <plugins>\ProcessNetMonitor\lang\
if not exist "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor\plugins\ProcessNetMonitor\lang" mkdir "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor\plugins\ProcessNetMonitor\lang"
copy /y src\lang\*.ini "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor\plugins\ProcessNetMonitor\lang\" >nul
:: Start TM as user Yang (must use working directory for TM to find its config)
echo @echo off > "%TEMP%\launch_tm.bat"
echo cd /d "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor" >> "%TEMP%\launch_tm.bat"
echo start "" TrafficMonitor.exe >> "%TEMP%\launch_tm.bat"
schtasks /create /tn "LaunchTM" /tr "%TEMP%\launch_tm.bat" /sc once /st 00:00 /ru Yang /f >nul 2>&1
schtasks /run /tn "LaunchTM" >nul 2>&1
timeout /t 3 /nobreak >nul
schtasks /delete /tn "LaunchTM" /f >nul 2>&1
echo DEPLOYED OK (x64)
goto :eof

:deploy_all
:: Deploy both DLLs (kill+poll first - see :deploy note)
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"name='TrafficMonitor.exe'\" | Invoke-CimMethod -MethodName Terminate | Out-Null; foreach ($i in 1..30) { Start-Sleep -Seconds 1; if (-not (Get-CimInstance Win32_Process -Filter \"name='TrafficMonitor.exe'\")) { exit 0 } }; exit 1"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: TrafficMonitor did not exit within 30s, deploy aborted
    exit /b 1
)
copy /y ProcessNetMonitor.dll "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor\plugins\ProcessNetMonitor.dll"
copy /y ProcessNetMonitor_x86.dll "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor\plugins\ProcessNetMonitor_x86.dll"
if %ERRORLEVEL% NEQ 0 (
    echo WARNING: DLL copy failed! Close TrafficMonitor first.
    exit /b 1
)
:: Deploy language files to <plugins>\ProcessNetMonitor\lang\
if not exist "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor\plugins\ProcessNetMonitor\lang" mkdir "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor\plugins\ProcessNetMonitor\lang"
copy /y src\lang\*.ini "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor\plugins\ProcessNetMonitor\lang\" >nul
:: Start TM as user Yang
echo @echo off > "%TEMP%\launch_tm.bat"
echo cd /d "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\TrafficMonitor\TrafficMonitor" >> "%TEMP%\launch_tm.bat"
echo start "" TrafficMonitor.exe >> "%TEMP%\launch_tm.bat"
schtasks /create /tn "LaunchTM" /tr "%TEMP%\launch_tm.bat" /sc once /st 00:00 /ru Yang /f >nul 2>&1
schtasks /run /tn "LaunchTM" >nul 2>&1
timeout /t 3 /nobreak >nul
schtasks /delete /tn "LaunchTM" /f >nul 2>&1
echo DEPLOYED OK (x86 + x64)
goto :eof
