@echo off
rem Builds the PES13 runtime + recompiled code (Windows x64, clang-cl + lld-link + Ninja).
rem Usage: tools\build-runtime.bat [Release|RelWithDebInfo|Debug]   (default: Release)
rem Output: build\<config>\bin\pes13.exe   Log: build\<config>\build.log
setlocal
set "ROOT=%~dp0.."
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
set "OUT=%ROOT%\build\%CONFIG%"

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio not found & exit /b 1)
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul || exit /b 1

if not exist "%OUT%" mkdir "%OUT%"
echo Configuring (%CONFIG%)...
cmake -S "%ROOT%" -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% ^
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link > "%OUT%\build.log" 2>&1 || goto :fail

echo Building - this takes a while the first time (431 generated files). Progress: %OUT%\build.log
cmake --build "%OUT%" --target pes13 >> "%OUT%\build.log" 2>&1 || goto :fail

echo.
echo BUILD OK: %OUT%\bin\pes13.exe
exit /b 0

:fail
echo.
echo BUILD FAILED - last lines of %OUT%\build.log:
powershell -NoProfile -Command "Get-Content '%OUT%\build.log' -Tail 30"
exit /b 1
