@echo off
rem Builds XenosRecomp (Xenos shader -> HLSL -> SPIR-V). DXIL output is disabled:
rem the renderer is Vulkan-only (MoltenVK on iPad).
setlocal
set "SRC=%~dp0XenosRecomp"
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio not found & exit /b 1)
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul || exit /b 1
cmake -S "%SRC%" -B "%SRC%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DXENOS_RECOMP_DXIL=OFF || exit /b 1
cmake --build "%SRC%\build" --target XenosRecomp || exit /b 1
echo.
echo BUILD OK
