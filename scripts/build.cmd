@echo off
setlocal EnableExtensions

rem Build the complete HIDMaestro UMDF2 HID driver source as xiaoji.
rem Usage: scripts\build.cmd [x64] [package | driver [package]]

for %%D in ("%~dp0..") do set "XIAOJI_ROOT=%%~fD"
set "DRIVER_DIR=%XIAOJI_ROOT%\driver"
set "HOST_DIR=%XIAOJI_ROOT%\host"
set "OUT_DIR=%XIAOJI_ROOT%\build"
if /i "%~2"=="driver" set "OUT_DIR=%XIAOJI_ROOT%\x64\driver"
if defined XIAOJI_OUT_DIR for %%D in ("%XIAOJI_OUT_DIR%") do set "OUT_DIR=%%~fD"
set "ARCH=%~1"
if not defined ARCH set "ARCH=x64"
if /i not "%ARCH%"=="x64" (
    echo ERROR: xiaoji currently supports x64 only.
    exit /b 1
)

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_PATH="
if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%I"
if not defined VS_PATH if defined VSINSTALLDIR set "VS_PATH=%VSINSTALLDIR%"
if not defined VS_PATH (
    echo ERROR: Visual Studio C++ build tools were not found. Set VSINSTALLDIR or install the MSVC x64 workload.
    exit /b 1
)

set "SDK_ROOT=%XIAOJI_WINDOWS_SDK_ROOT%"
if not defined SDK_ROOT if defined WindowsSdkDir set "SDK_ROOT=%WindowsSdkDir%"
if not defined SDK_ROOT for /f "usebackq delims=" %%I in (`powershell.exe -NoProfile -NonInteractive -Command "(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').PSObject.Properties | Where-Object Name -like 'Kits*10' | Select-Object -First 1 -ExpandProperty Value"`) do set "SDK_ROOT=%%I"
if not defined SDK_ROOT set "SDK_ROOT=%ProgramFiles(x86)%\Windows Kits\10"
if "%SDK_ROOT:~-1%"=="\" set "SDK_ROOT=%SDK_ROOT:~0,-1%"

set "WDK_ROOT=%XIAOJI_WDK_ROOT%"
if not defined WDK_ROOT set "WDK_ROOT=%SDK_ROOT%"
if "%WDK_ROOT:~-1%"=="\" set "WDK_ROOT=%WDK_ROOT:~0,-1%"

set "KIT_VER=%XIAOJI_WDK_VERSION%"
if not defined KIT_VER (
    for /f "delims=" %%V in ('dir /b /ad /o-n "%SDK_ROOT%\Include\10.*" 2^>nul') do (
        if not defined KIT_VER if exist "%SDK_ROOT%\Include\%%V\um\windows.h" if exist "%WDK_ROOT%\Include\%%V\km\hidport.h" if exist "%WDK_ROOT%\Include\wdf\umdf\2.15\wdf.h" set "KIT_VER=%%V"
    )
)

if not defined KIT_VER (
    echo ERROR: No usable Windows SDK/WDK version was found under "%SDK_ROOT%" and "%WDK_ROOT%".
    exit /b 1
)
if not exist "%WDK_ROOT%\Lib\wdf\umdf\x64\2.15\WdfDriverStubUm.lib" (
    echo ERROR: UMDF 2.15 x64 libraries are missing from "%WDK_ROOT%".
    exit /b 1
)

set "VCVARS=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo ERROR: vcvars64.bat was not found under "%VS_PATH%".
    exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

set "UM_INC=%SDK_ROOT%\Include\%KIT_VER%\um"
set "SHARED_INC=%SDK_ROOT%\Include\%KIT_VER%\shared"
set "KM_INC=%WDK_ROOT%\Include\%KIT_VER%\km"
set "WDF_INC=%WDK_ROOT%\Include\wdf\umdf\2.15"
set "RC=%SDK_ROOT%\bin\%KIT_VER%\x64\rc.exe"
if not exist "%RC%" set "RC=%WDK_ROOT%\bin\%KIT_VER%\x64\rc.exe"

echo xiaoji UMDF2 HID driver build
echo   driver : %DRIVER_DIR%\xiaoji.c
echo   SDK    : %SDK_ROOT% (%KIT_VER%)
echo   WDK    : %WDK_ROOT%
echo   output : %OUT_DIR%

cl.exe /nologo /W4 /GS /Gz /Ox /Os /D _AMD64_ /D _WIN64 /D UNICODE /D _UNICODE /D UMDF_VERSION_MAJOR=2 /D UMDF_VERSION_MINOR=15 ^
    /I"%UM_INC%" /I"%SHARED_INC%" /I"%KM_INC%" /external:I"%WDF_INC%" /external:W0 /I"%DRIVER_DIR%" /I"%OUT_DIR%" ^
    /Fo"%OUT_DIR%\\" /c "%DRIVER_DIR%\xiaoji.c"
if errorlevel 1 exit /b 1

"%RC%" /nologo /fo "%OUT_DIR%\res_xiaoji.res" /I"%OUT_DIR%" /I"%DRIVER_DIR%" /I"%UM_INC%" /I"%SHARED_INC%" "%DRIVER_DIR%\res_xiaoji.rc"
if errorlevel 1 exit /b 1

link.exe /nologo /DLL /MACHINE:X64 /OUT:"%OUT_DIR%\xiaoji.dll" ^
    /LIBPATH:"%SDK_ROOT%\Lib\%KIT_VER%\um\x64" ^
    /LIBPATH:"%WDK_ROOT%\Lib\wdf\umdf\x64\2.15" ^
    "%OUT_DIR%\xiaoji.obj" "%OUT_DIR%\res_xiaoji.res" ^
    WdfDriverStubUm.lib ntdll.lib OneCoreUAP.lib mincore.lib advapi32.lib
if errorlevel 1 exit /b 1

copy /y "%DRIVER_DIR%\xiaoji.inf" "%OUT_DIR%\xiaoji.inf" >nul
if errorlevel 1 exit /b 1

if /i "%~2"=="driver" (
    echo DRIVER BUILT: "%OUT_DIR%\xiaoji.dll"
    echo INF         : "%OUT_DIR%\xiaoji.inf"
    if /i "%~3"=="package" goto :catalog
    exit /b 0
)

echo Compiling xiaoji-host.exe ...
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -File "%~dp0build_setup.ps1" -OutDir "%OUT_DIR%" -SdkBin "%SDK_ROOT%\bin\%KIT_VER%\x64" -WdkBin "%WDK_ROOT%\bin\%KIT_VER%\x86"
if errorlevel 1 exit /b 1
"%RC%" /nologo /fo "%OUT_DIR%\setup.res" /I"%OUT_DIR%" "%HOST_DIR%\setup.rc"
if errorlevel 1 exit /b 1
cl.exe /nologo /W4 /EHsc /std:c++17 /D UNICODE /D _UNICODE ^
    /Fo"%OUT_DIR%\local_install.obj" /c "%HOST_DIR%\local_install.cpp"
if errorlevel 1 exit /b 1
cl.exe /nologo /W4 /EHsc /std:c++17 /D UNICODE /D _UNICODE /I"%HOST_DIR%" ^
    /Fo"%OUT_DIR%\xiaoji_host.obj" /c "%HOST_DIR%\xiaoji_host.cpp"
if errorlevel 1 exit /b 1

link.exe /nologo /SUBSYSTEM:CONSOLE /MACHINE:X64 /OUT:"%OUT_DIR%\xiaoji-host.exe" ^
    "%OUT_DIR%\xiaoji_host.obj" "%OUT_DIR%\local_install.obj" "%OUT_DIR%\setup.res" setupapi.lib cfgmgr32.lib newdev.lib advapi32.lib shell32.lib ole32.lib uuid.lib user32.lib
if errorlevel 1 exit /b 1

echo Compiling xiaoji-test.exe GUI ...
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -File "%~dp0build_setup.ps1" -OutDir "%OUT_DIR%" -SdkBin "%SDK_ROOT%\bin\%KIT_VER%\x64" -WdkBin "%WDK_ROOT%\bin\%KIT_VER%\x86" -Gui
if errorlevel 1 exit /b 1
echo BUILD SUCCEEDED: %OUT_DIR%\xiaoji.dll
echo HOST            : %OUT_DIR%\xiaoji-host.exe
echo TEST            : %OUT_DIR%\xiaoji-test.exe

if /i not "%~2"=="package" exit /b 0

:catalog
set "INF2CAT=%WDK_ROOT%\bin\%KIT_VER%\x86\Inf2Cat.exe"
if not exist "%INF2CAT%" (
    echo ERROR: Inf2Cat.exe was not found under "%WDK_ROOT%".
    exit /b 1
)
"%INF2CAT%" /driver:"%OUT_DIR%" /os:10_X64
if errorlevel 1 exit /b 1
echo PACKAGE CATALOG: %OUT_DIR%\xiaoji.cat

exit /b 0
