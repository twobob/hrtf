@echo off
setlocal enabledelayedexpansion

:: ---------------------------------------------------------------
:: Fail fast if the vendored SDK submodules are not checked out.
:: ---------------------------------------------------------------
if not exist "public.sdk\source\vst\vstaudioeffect.h" goto missing_submodules
if not exist "base\source\baseiids.cpp"               goto missing_submodules
if not exist "pluginterfaces\base\funknown.cpp"       goto missing_submodules
if not exist "clap-src\include\clap\clap.h"           goto missing_submodules

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

if not exist "build" mkdir "build"

echo ========================================================
echo 1. Building Rotating HRTF v2 CLAP Plugin...
echo ========================================================

cl.exe /nologo /LD /O2 /std:c11 /Iclap-src\include /D_CRT_SECURE_NO_WARNINGS ^
    hrtf_core.c hrtf_clap.c ^
    /Fo:build\ /Fe:RotatingHRTF_v2.clap
if errorlevel 1 (
    echo ERROR: Failed to build CLAP plugin!
    exit /b 1
)

echo.
echo ========================================================
echo 2. Building Rotating HRTF v2 VST3 Plugin (for Ableton Live)...
echo ========================================================

set VST3_SOURCES=^
    hrtf_core.c ^
    vst3\plugprocessor.cpp ^
    vst3\plugcontroller.cpp ^
    vst3\plugfactory.cpp ^
    public.sdk\source\main\dllmain.cpp ^
    public.sdk\source\main\moduleinit.cpp ^
    public.sdk\source\main\pluginfactory.cpp ^
    public.sdk\source\vst\vstaudioeffect.cpp ^
    public.sdk\source\vst\vstbus.cpp ^
    public.sdk\source\vst\vstcomponent.cpp ^
    public.sdk\source\vst\vstcomponentbase.cpp ^
    public.sdk\source\vst\vsteditcontroller.cpp ^
    public.sdk\source\vst\vstinitiids.cpp ^
    public.sdk\source\vst\vstparameters.cpp ^
    public.sdk\source\vst\utility\stringconvert.cpp ^
    public.sdk\source\common\commonstringconvert.cpp ^
    public.sdk\source\common\commoniids.cpp ^
    public.sdk\source\common\openurl.cpp ^
    public.sdk\source\common\pluginview.cpp ^
    pluginterfaces\base\conststringtable.cpp ^
    pluginterfaces\base\coreiids.cpp ^
    pluginterfaces\base\funknown.cpp ^
    pluginterfaces\base\ustring.cpp ^
    base\source\baseiids.cpp ^
    base\source\fbuffer.cpp ^
    base\source\fdebug.cpp ^
    base\source\fobject.cpp ^
    base\source\fstreamer.cpp ^
    base\source\fstring.cpp ^
    base\source\timer.cpp ^
    base\source\updatehandler.cpp ^
    base\thread\source\flock.cpp

cl.exe /nologo /LD /O2 /std:c++17 /EHsc /MD /I. /Ivst3 /DRELEASE=1 /D_CRT_SECURE_NO_WARNINGS ^
    %VST3_SOURCES% ^
    /Fo:build\ ^
    /Fe:RotatingHRTF_v2.vst3 /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib ole32.lib
if errorlevel 1 (
    echo ERROR: Failed to build VST3 plugin!
    exit /b 1
)

echo.
echo Creating VST3 standard bundle directory structure...
if not exist "bundle\RotatingHRTF_v2.vst3\Contents\x86_64-win" (
    mkdir "bundle\RotatingHRTF_v2.vst3\Contents\x86_64-win"
)
copy /y RotatingHRTF_v2.vst3 "bundle\RotatingHRTF_v2.vst3\Contents\x86_64-win\RotatingHRTF_v2.vst3"

echo.
echo ========================================================
echo 3. Building and running automated tests...
echo ========================================================

cl.exe /nologo /O2 /Iclap-src\include test_clap.c /Fo:build\ /Fe:test_clap.exe
if errorlevel 1 (
    echo ERROR: Failed to build test_clap.exe!
    exit /b 1
)
test_clap.exe
if errorlevel 1 (
    echo ERROR: test_clap.exe reported failures!
    exit /b 1
)

cl.exe /nologo /O2 /std:c++17 /EHsc /MD /I. /Ivst3 /DRELEASE=1 test_vst3.cpp pluginterfaces\base\funknown.cpp public.sdk\source\vst\vstinitiids.cpp ole32.lib /Fo:build\ /Fe:test_vst3.exe
if errorlevel 1 (
    echo ERROR: Failed to build test_vst3.exe!
    exit /b 1
)
test_vst3.exe
if errorlevel 1 (
    echo ERROR: test_vst3.exe reported failures!
    exit /b 1
)

echo.
echo ========================================================
echo 4. Deploying to the system VST3 folder...
echo ========================================================

set TARGET_VST3_DIR=C:\Program Files\Common Files\VST3\RotatingHRTF_v2.vst3\Contents\x86_64-win
if not exist "%TARGET_VST3_DIR%" mkdir "%TARGET_VST3_DIR%" 2>nul
copy /y RotatingHRTF_v2.vst3 "%TARGET_VST3_DIR%\RotatingHRTF_v2.vst3" >nul
if errorlevel 1 (
    echo NOT DEPLOYED: could not write to "%TARGET_VST3_DIR%".
    echo               Run install_ableton.bat as Administrator to install.
) else (
    echo Deployed to: %TARGET_VST3_DIR%\RotatingHRTF_v2.vst3
)

echo.
echo ========================================================
echo BUILD AND TEST COMPLETED SUCCESSFULLY
echo Artifacts ready:
echo   - RotatingHRTF_v2.clap (CLAP plugin for Bitwig, Reaper, etc.)
echo   - RotatingHRTF_v2.vst3 (VST3 plugin for Ableton Live)
echo   - bundle\RotatingHRTF_v2.vst3\ (VST3 bundle structure)
echo ========================================================

exit /b 0

:missing_submodules
echo.
echo ERROR: vendored SDK submodules are missing from this checkout.
echo        Fix with:  git submodule update --init --recursive
echo.
exit /b 1
