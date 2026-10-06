@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio was not found.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo The Desktop development with C++ workload was not found.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release || cmake -S "%~dp0." -B "%~dp0build" -A x64
if errorlevel 1 exit /b 1
cmake --build "%~dp0build" --config Release
if errorlevel 1 exit /b 1
echo.
echo Built build\bin\mirror_host.exe
echo Run it from a terminal. Click VIDEO, PAGE, or SCENE.
