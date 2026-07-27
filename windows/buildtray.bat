@echo off
REM Build ONLY CatCamTray.exe (host/DLL binaries may be running and locked).
setlocal
cd /d %~dp0
set VCVARS=
for %%e in (Community Professional Enterprise BuildTools) do if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (echo ERROR: Visual Studio 2022 C++ build tools not found & exit /b 1)
call "%VCVARS%" >nul

set SDK_INC=C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0
set SDK_LIB=C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0

set DEFS=/D _WIN32_WINNT=0x0A00 /D NTDDI_VERSION=0x0A00000B /D UNICODE /D _UNICODE /D WIN32_LEAN_AND_MEAN
set INCS=/I "%SDK_INC%\um" /I "%SDK_INC%\shared" /I "%SDK_INC%\ucrt"
set LIBDIRS=/LIBPATH:"%SDK_LIB%\um\x64" /LIBPATH:"%SDK_LIB%\ucrt\x64"

cl /nologo /std:c++17 /EHsc /O2 %DEFS% %INCS% ^
   CatCamTray.cpp /Fe:CatCamTray.exe /link %LIBDIRS% /MANIFESTUAC:NO ^
   /MANIFEST:EMBED /MANIFESTINPUT:CatCamTray.manifest ^
   user32.lib shell32.lib gdi32.lib gdiplus.lib advapi32.lib || exit /b 1

echo === TRAY DONE ===
endlocal
