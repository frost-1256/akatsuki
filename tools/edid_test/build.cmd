@echo off
rem Build the host EDID test with the EWDK (x64 host compiler). Usage: build.cmd <EWDK drive letter, e.g. H>
setlocal
call %1:\BuildEnv\SetupBuildEnv.cmd amd64 >nul 2>&1
rem The EWDK build env targets drivers; add the user-mode UCRT/UM paths for this host tool.
set "K=%1:\Program Files\Windows Kits\10"
set "INCLUDE=%INCLUDE%;%K%\Include\10.0.28000.0\ucrt;%K%\Include\10.0.28000.0\um;%K%\Include\10.0.28000.0\shared"
set "LIB=%LIB%;%K%\Lib\10.0.28000.0\ucrt\x64;%K%\Lib\10.0.28000.0\um\x64"
set OUT=%~dp0..\..\build\edid_test
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /EHsc /W4 /WX /std:c++17 /Fo"%OUT%\\" /Fe"%OUT%\edid_test.exe" "%~dp0edid_test.cxx" "%~dp0..\..\src\epinephrine\epi_edid.cxx"
