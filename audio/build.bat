@echo off
REM Build CatCamMic.sys (SYSVAD-derived virtual mic) with WDK toolchain directly.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul

set KIT=C:\Program Files (x86)\Windows Kits\10
set VER=10.0.26100.0
set KMINC=%KIT%\Include\%VER%\km
set KMINC_CRT=%KIT%\Include\%VER%\km\crt
set SHARED=%KIT%\Include\%VER%\shared
set UM=%KIT%\Include\%VER%\um
set UCRT=%KIT%\Include\%VER%\ucrt
set KMLIB=%KIT%\Lib\%VER%\km\x64
set UCRTLIB=%KIT%\Lib\%VER%\ucrt\x64
set UMLIB=%KIT%\Lib\%VER%\um\x64

cd /d C:\Users\b4rru\catcam\audio\src
if exist out del /q out\*.obj 2>nul
mkdir out 2>nul

set DEFS=/DPOOL_NX_OPTIN=1 /D_AMD64_ /DAMD64 /DWINVER=0x0A00 /D_WIN32_WINNT=0x0A00 /D_WIN64 /DNTDDI_VERSION=0x0A00000C /DUNICODE /D_UNICODE /DNDEBUG /D_USE_WAVERT_ /DSYSVAD_BTH_BYPASS /DSYSVAD_USB_SIDEBAND /D_NEW_DELETE_OPERATORS_
set INCS=/I. /I"%KMINC%" /I"%KMINC_CRT%" /I"%SHARED%" /I"%UM%" /I"%UCRT%" /I"%KIT%\Include\wdf\kmdf\1.31"
set CFLAGS=/nologo /W3 /WX- /EHsc /std:c++17 /kernel /GF /GR- /Zp8 /GS- /Zc:wchar_t- /Od /c /Fo:out\

echo === compile ===
REM Exclude user-mode KeywordDetector DLL stubs (dllmain/stdafx) — kernel build only.
for %%f in (*.cpp) do (
  if /I not "%%f"=="dllmain.cpp" if /I not "%%f"=="stdafx.cpp" (
    cl %CFLAGS% %DEFS% %INCS% %%f || exit /b 1
  )
)

echo === link ===
link /nologo /DRIVER /SUBSYSTEM:NATIVE /ENTRY:GsDriverEntry /NODEFAULTLIB ^
  /OUT:out\CatCamMic.sys out\*.obj ^
  /LIBPATH:"%KMLIB%" /LIBPATH:"%UCRTLIB%" /LIBPATH:"%UMLIB%" ^
  /LIBPATH:"%KIT%\Lib\wdf\kmdf\x64\1.31" ^
  portcls.lib drmk.lib stdunk.lib ksecdd.lib ntoskrnl.lib hal.lib wmilib.lib BufferOverflowK.lib libcntpr.lib wdfdriverentry.lib wdfldr.lib || exit /b 1

echo === DONE ===
endlocal
