@echo off
rem Builds every DLL a release ships, as 32-bit DLLs with MSVC (Visual Studio 2022 Build Tools), into build\win:
rem   harness.dll      the op2ext module (src\harness.cpp)
rem   zScenario.dll    the scenario mission (test\Scenario.cpp)
rem   zHarnessEnd.dll  the self-test mission, won at tick 800 (test\EndTest.cpp)
rem   *_bot.dll        the measurement bots (bots\*.cpp)
rem The same set as the Makefile's `all`. The sources are written for MinGW GCC; windows\msvc_compat.h is
rem force-included to cover the differences. Needs TethysAPI 141fd47 at vendor\include\Tethys (windows\README.md).
rem /Brepro makes builds reproducible (no timestamps): the same sources give byte-identical DLLs.
rem Each DLL's compiler output is in build\win\<name>.log.
setlocal
set ROOT=%~dp0..
if not exist "%ROOT%\vendor\include\Tethys\API\API.h" (echo build: TethysAPI missing at vendor\include\Tethys & exit /b 1)
rem Visual Studio with the C++ tools, found by vswhere (Build Tools, Community, Enterprise...); VCVARS overrides it.
if defined VCVARS goto have_vcvars
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (echo build: no vswhere: install Visual Studio 2022 Build Tools, C++ workload & exit /b 1)
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%I
if not defined VSPATH (echo build: no Visual Studio with the C++ tools found & exit /b 1)
set VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars32.bat
:have_vcvars
if not exist "%VCVARS%" (echo build: no "%VCVARS%" & exit /b 1)
echo build: using %VCVARS%
call "%VCVARS%" >nul 2>&1
if not exist "%ROOT%\build\win\obj" mkdir "%ROOT%\build\win\obj"
cd /d "%ROOT%\build\win"
set FLAGS=/nologo /LD /O2 /std:c++20 /EHsc /MT /W1 /utf-8 /I"%ROOT%\vendor\include" /I"%ROOT%\windows" /FImsvc_compat.h /Brepro /Foobj\
set LIBS=user32.lib gdi32.lib winmm.lib ws2_32.lib
set FAILED=
call :build harness "%ROOT%\src\harness.cpp"
call :build zScenario "%ROOT%\test\Scenario.cpp"
call :build zHarnessEnd "%ROOT%\test\EndTest.cpp"
for %%B in (sample easter cursor hpblink hitmask los edge) do call :build %%B_bot "%ROOT%\bots\%%B_bot.cpp"
if defined FAILED (echo build: FAILED:%FAILED% & exit /b 1)
echo build: 10 DLLs in build\win (harness, zScenario, zHarnessEnd and 7 bots)
exit /b 0

:build
cl %FLAGS% %2 /Fe:%1.dll /link /Brepro %LIBS% > %1.log 2>&1
if errorlevel 1 (echo build: %1.dll failed, see build\win\%1.log & set FAILED=%FAILED% %1)
exit /b 0
