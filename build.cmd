@echo off
setlocal
title Evolution build

set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" set "MSBUILD=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\MSBuild.exe"

if not exist "%MSBUILD%" (
    echo [!] MSBuild was not found. Install Visual Studio 2022 with "Desktop development with C++".
    pause
    exit /b 1
)

if not exist "%~dp0wdk\nuget\.extracted" (
    echo [~] WDK not found locally, downloading it once ^(no admin rights needed^)...
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-wdk.ps1"
    if errorlevel 1 (
        echo [!] WDK setup failed
        pause
        exit /b 1
    )
)

echo [~] building Evolution ^(Release x64^): loader + kernel driver...
"%MSBUILD%" "%~dp0Evolution.sln" /t:Rebuild /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
if errorlevel 1 (
    echo [!] build failed
    pause
    exit /b 1
)

echo.
echo [+] build ok:
echo     loader : %~dp0output\x64\Release\Evolution.exe
echo     driver : %~dp0output\x64\Release\evolution.sys
echo.
echo [~] run Evolution.exe as administrator and choose "1 - Start"
pause
exit /b 0
