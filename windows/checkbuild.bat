@echo off
REM Compile pre-check with scratch output names: verifies everything builds
REM WITHOUT touching the running binaries, so fullredeploy only takes the
REM camera down once the build is known-good.
setlocal
cd /d %~dp0
set VCVARS=
for %%e in (Community Professional Enterprise BuildTools) do if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (echo ERROR: Visual Studio 2022 C++ build tools not found & exit /b 1)
call "%VCVARS%" >nul

REM Auto-detect the newest installed Windows SDK (any machine, any
REM version; the last name-sorted entry wins = highest modern build).
set "SDK_ROOT=C:\Program Files (x86)\Windows Kits\10"
set SDK_VER=
for /f "delims=" %%v in ('dir /b /ad /o:n "%SDK_ROOT%\Include" 2^>nul') do set SDK_VER=%%v
if not defined SDK_VER (echo ERROR: Windows SDK not found under "%SDK_ROOT%\Include" & exit /b 1)
set "SDK_INC=%SDK_ROOT%\Include\%SDK_VER%"
set "SDK_LIB=%SDK_ROOT%\Lib\%SDK_VER%"

set DEFS=/D _WIN32_WINNT=0x0A00 /D NTDDI_VERSION=0x0A00000B /D UNICODE /D _UNICODE /D WIN32_LEAN_AND_MEAN
set INCS=/I "%SDK_INC%\um" /I "%SDK_INC%\shared" /I "%SDK_INC%\ucrt"
set LIBDIRS=/LIBPATH:"%SDK_LIB%\um\x64" /LIBPATH:"%SDK_LIB%\ucrt\x64"

cl /nologo /std:c++17 /EHsc /LD %DEFS% %INCS% ^
   DllMain.cpp MediaSource.cpp MediaStream.cpp FrameServer.cpp ^
   /Fe:chk_CatCamSource.dll /link %LIBDIRS% /DEF:CatCamSource.def ^
   /IMPLIB:chk_CatCamSource.lib ^
   mf.lib mfplat.lib mfuuid.lib user32.lib ole32.lib advapi32.lib || exit /b 1

cl /nologo /std:c++17 /EHsc %DEFS% %INCS% ^
   CatCamHost.cpp /Fe:chk_CatCamHost.exe /link %LIBDIRS% ^
   mf.lib mfplat.lib mfuuid.lib mfsensorgroup.lib ole32.lib ws2_32.lib || exit /b 1

cl /nologo /std:c++17 /EHsc /O2 %DEFS% %INCS% ^
   CatCamTray.cpp /Fe:chk_CatCamTray.exe /link %LIBDIRS% /MANIFESTUAC:NO ^
   /MANIFEST:EMBED /MANIFESTINPUT:CatCamTray.manifest ^
   user32.lib shell32.lib gdi32.lib gdiplus.lib advapi32.lib ws2_32.lib || exit /b 1

del chk_CatCamSource.dll chk_CatCamSource.lib chk_CatCamSource.exp chk_CatCamHost.exe chk_CatCamTray.exe 2>nul
echo === CHECK OK ===
endlocal
