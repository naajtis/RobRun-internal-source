@echo off
setlocal
cd /d "%~dp0"
set "VS="
for %%P in (
    "C:\Program Files\Microsoft Visual Studio\18\Community"
    "C:\Program Files\Microsoft Visual Studio\18\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Community"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional"
) do if not defined VS if exist "%%~P\VC\Auxiliary\Build\vcvars64.bat" set "VS=%%~P"
if not defined VS (echo VS_NOT_FOUND & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "build" mkdir "build" || exit /b 1
cl /nologo /O2 /W3 /MT /D_CRT_SECURE_NO_WARNINGS /I . ^
    tools\rr_injector.c internal\rr_map.c /Fe:build\RR-Injector.exe ^
    /link kernel32.lib advapi32.lib || exit /b 1
echo BUILD_OK: build\RR-Injector.exe
