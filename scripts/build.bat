@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "CMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "PATH=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set ROOT=%~dp0..
pushd "%ROOT%"
if not exist "build" mkdir "build"
cd build
"%CMAKE%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="%NINJA%" "%CD%\.."
if errorlevel 1 exit /b 1
"%CMAKE%" --build . --config Release
set ERR=%ERRORLEVEL%
popd
exit /b %ERR%
