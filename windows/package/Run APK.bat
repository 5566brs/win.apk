@echo off
rem Win.apk preview launcher. Drag an .apk file onto this file to run it.
setlocal
if "%~1"=="" (
    echo Drag an .apk file onto "Run APK.bat" to run it.
    pause
    exit /b 1
)
set "LOGDIR=%LOCALAPPDATA%\Win.apk\logs"
if not exist "%LOGDIR%" mkdir "%LOGDIR%"
set "LOG=%LOGDIR%\last-run.log"
echo Starting %~nx1 ...
echo The first start of an app converts its code and takes longer. Keep this window open.
"%~dp0bin\winapk-runtime.exe" --apk "%~f1" > "%LOG%" 2>&1
if errorlevel 1 (
    echo.
    echo Win.apk stopped with an error. The log is in:
    echo %LOG%
    pause
)
