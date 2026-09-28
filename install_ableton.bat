@echo off
setlocal

:: ---------------------------------------------------------------
:: Installs the built VST3 bundle so DAWs (Ableton Live, etc.) find it.
::
::   Usage:  install_ableton.bat [vst3-root]
;;
:: The default root is "C:\Program Files\Common Files\VST3", which needs
:: Administrator rights, so the script re-launches itself elevated.
:: Pass a different root (e.g. a user folder) to install without UAC.
::
:: The script verifies the copy before reporting success and exits with
:: a non-zero status when nothing was installed.
:: ---------------------------------------------------------------

set "VST3_ROOT=C:\Program Files\Common Files\VST3"
if not "%~1"=="" set "VST3_ROOT=%~1"

:: Set by the elevated re-launch so the child does not pause twice.
set "QUIET="
if /i "%~2"=="quiet" set "QUIET=1"

set "BUNDLE=%VST3_ROOT%\RotatingHRTF_v2.vst3"
set "TARGET_DIR=%BUNDLE%\Contents\x86_64-win"
set "TARGET=%TARGET_DIR%\RotatingHRTF_v2.vst3"
set "SOURCE=%~dp0RotatingHRTF_v2.vst3"

echo ==========================================================
echo Installing Rotating HRTF v2 VST3
echo ==========================================================
echo   Source: %SOURCE%
echo   Target: %TARGET%
echo.

if not exist "%SOURCE%" (
    echo ERROR: "%SOURCE%" not found.
    echo        Run build_all.bat first to produce it.
    goto fail
)

mkdir "%TARGET_DIR%" 2>nul
if not exist "%TARGET_DIR%" (
    :: Never re-elevate from the elevated instance, or a target that cannot
    :: be created would bounce UAC prompts forever.
    if defined QUIET goto fail
    echo Administrator rights are required to write to "%VST3_ROOT%".
    echo Elevating - accept the UAC prompt in the window that opens.
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs -ArgumentList '%VST3_ROOT%','quiet' -Wait"
    :: The elevated instance performs the copy; verify it really happened.
    if not exist "%TARGET%" goto fail
    goto installed
)

copy /y "%SOURCE%" "%TARGET%" >nul
if errorlevel 1 (
    echo ERROR: failed to copy to "%TARGET%".
    goto fail
)

:installed
echo.
echo ==========================================================
echo Installation successful
echo   %TARGET%
echo.
echo In Ableton Live:
echo   1. Preferences -^> Plug-Ins
echo   2. Make sure 'Use VST3 Plug-In System Folders' is ON
echo   3. Click 'Rescan Plug-ins' (hold Alt for a deep rescan)
echo   4. Look for 'Rotating HRTF v2' under Plug-Ins -^> VST3
echo ==========================================================
if not defined QUIET pause
exit /b 0

:fail
echo.
echo INSTALLATION FAILED - nothing was installed.
if not defined QUIET pause
exit /b 1
