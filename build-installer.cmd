@echo off
setlocal
cd /d "%~dp0"

if not exist "ScreenRec-x64.exe" (
  echo Сначала соберите программу: build.cmd
  exit /b 1
)

set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
  echo Не найден компилятор Inno Setup 6
  exit /b 1
)

"%ISCC%" "installer\ScreenRec.iss" || exit /b 1
echo Установщик: dist\ScreenRec-1.0.0-x64-setup.exe
