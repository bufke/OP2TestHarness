@echo off
rem Builds harness.dll and zScenario.dll as 32-bit DLLs with MSVC (Visual Studio 2022 Build Tools) into build\win.
rem The sources are written for MinGW GCC; windows\msvc_compat.h is force-included to cover the differences.
rem Needs TethysAPI 141fd47 at vendor\include\Tethys (windows\README.md).
setlocal
set ROOT=%~dp0..
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat
if not exist "%ROOT%\vendor\include\Tethys\API\API.h" (echo build: TethysAPI missing at vendor\include\Tethys & exit /b 1)
call "%VCVARS%" >nul 2>&1
if not exist "%ROOT%\build\win" mkdir "%ROOT%\build\win"
cd /d "%ROOT%\build\win"
set FLAGS=/nologo /LD /O2 /std:c++20 /EHsc /MT /W1 /utf-8 /I"%ROOT%\vendor\include" /I"%ROOT%\windows" /FImsvc_compat.h
cl %FLAGS% "%ROOT%\src\harness.cpp" /Fe:harness.dll /link user32.lib gdi32.lib winmm.lib ws2_32.lib > harness_build.log 2>&1
if errorlevel 1 (echo build: harness.dll failed, see build\win\harness_build.log & exit /b 1)
cl %FLAGS% "%ROOT%\test\Scenario.cpp" /Fe:zScenario.dll /link user32.lib > scenario_build.log 2>&1
if errorlevel 1 (echo build: zScenario.dll failed, see build\win\scenario_build.log & exit /b 1)
echo build: build\win\harness.dll and build\win\zScenario.dll
