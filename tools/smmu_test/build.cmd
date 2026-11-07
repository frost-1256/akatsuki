@echo off
rem Host-only tests; never access device hardware. Usage: build.cmd H
setlocal
call %1:\BuildEnv\SetupBuildEnv.cmd amd64 >nul 2>&1
@echo off
set "K=%1:\Program Files\Windows Kits\10"
set "INCLUDE=%INCLUDE%;%K%\Include\10.0.28000.0\ucrt;%K%\Include\10.0.28000.0\um;%K%\Include\10.0.28000.0\shared"
set "LIB=%LIB%;%K%\Lib\10.0.28000.0\ucrt\x64;%K%\Lib\10.0.28000.0\um\x64"
set "OUT=%~dp0..\..\build\smmu_test"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /EHsc /W4 /WX /std:c++17 /I"%~dp0." /Fo"%OUT%\\" /Fe"%OUT%\smmu_test.exe" "%~dp0smmu_test.cxx"
if errorlevel 1 exit /b 1
"%OUT%\smmu_test.exe"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /W4 /WX /std:c++17 /I"%~dp0." /Fo"%OUT%\\" /Fe"%OUT%\gmu_test.exe" "%~dp0gmu_test.cxx"
if errorlevel 1 exit /b 1
"%OUT%\gmu_test.exe"
exit /b %errorlevel%
