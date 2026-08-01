@echo off
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
cl /nologo /std:c++17 /EHsc %DEFS% %INCS% CatCamHost.cpp /Fe:CatCamHost.exe /link %LIBDIRS% mf.lib mfplat.lib mfuuid.lib mfsensorgroup.lib ole32.lib ws2_32.lib
