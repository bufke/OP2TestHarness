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
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat
if not exist "%ROOT%\vendor\include\Tethys\API\API.h" (echo build: TethysAPI missing at vendor\include\Tethys & exit /b 1)
if not exist "%VCVARS%" (echo build: no Visual Studio 2022 Build Tools at "%VCVARS%" & exit /b 1)
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
