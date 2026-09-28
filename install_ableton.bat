@echo off
setlocal

echo ==========================================================
echo Installing Rotating HRTF VST3 for Ableton Live
echo ==========================================================

set TARGET_DIR=C:\Program Files\Common Files\VST3\RotatingHRTF.vst3\Contents\x86_64-win

echo Target directory: %TARGET_DIR%

:: Try creating directory
mkdir "%TARGET_DIR%" 2>nul
if errorlevel 1 (
    echo Administrator permissions required to copy to Program Files.
    echo Elevating to Administrator...
    powershell -Command "Start-Process cmd -ArgumentList '/c \"mkdir \"\"%TARGET_DIR%\"\" 2>nul & copy /y \"\"%~dp0RotatingHRTF.vst3\"\" \"\"%TARGET_DIR%\\RotatingHRTF.vst3\"\" & echo Installation successful! & pause\"' -Verb RunAs"
    goto finish
)

copy /y "%~dp0RotatingHRTF.vst3" "%TARGET_DIR%\RotatingHRTF.vst3"
if errorlevel 1 (
    echo Failed to copy. Trying elevated copy...
    powershell -Command "Start-Process cmd -ArgumentList '/c \"copy /y \"\"%~dp0RotatingHRTF.vst3\"\" \"\"%TARGET_DIR%\\RotatingHRTF.vst3\"\" & echo Installation successful! & pause\"' -Verb RunAs"
    goto finish
)

echo.
echo ==========================================================
echo Installation Successful!
echo Installed to: %TARGET_DIR%\RotatingHRTF.vst3
echo.
echo In Ableton Live:
echo 1. Open Preferences -> Plug-Ins
echo 2. Make sure 'Use VST3 Plug-In System Folders' is ON
echo 3. Click 'Rescan Plug-ins' (or hold Alt while clicking for deep rescan)
echo 4. Find 'Rotating HRTF' under Plug-Ins -> VST3 -> Example Audio
echo ==========================================================

:finish
pause
