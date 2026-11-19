@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\VC98\Bin\VCVARS32.BAT" >nul
set VC="C:\Program Files (x86)\Microsoft Visual Studio\VC98\Bin"

echo Building standalone cabarc (MSVC6 C89, no C++, no cabinet.dll)...

rem --- all sources are plain C89 ---
%VC%\CL.EXE /nologo /c /TC /W3 /O2 /I..\src ^
  ..\src\cabarc.c ..\src\cablib.c ..\src\qcabinet.c ..\src\quantum.c ^
  ..\src\inflate.c ..\src\msinflate.c ..\src\mszip.c ..\src\mszip_comp.c ^
  ..\src\lzencode.c ..\src\lzxenc.c ..\src\lzxdec.c
if errorlevel 1 goto fail

rem --- no cabinet.lib / cabinet.dll needed ---
%VC%\LINK.EXE /nologo /subsystem:console /out:cabarc_standalone.exe ^
  cabarc.obj cablib.obj qcabinet.obj quantum.obj inflate.obj msinflate.obj ^
  mszip.obj mszip_comp.obj lzencode.obj lzxenc.obj lzxdec.obj
if errorlevel 1 goto fail

echo Build succeeded: cabarc_standalone.exe (pure C89, standalone)
endlocal
exit /b 0

:fail
echo BUILD FAILED
endlocal
exit /b 1
