@echo off
rem Windows counterpart of third_party/build.sh: builds every third-party
rem dependency and merges the results into one third_party.a for the app to
rem link. Invoked by the root build.bat when necessary, or directly.
rem
rem Deviation from the mac script: no `make` on this platform, so instead of
rem raylib's Makefile we compile its seven unity sources directly with clang
rem (same result: one .o per source, PLATFORM_DESKTOP via GLFW/Win32) and
rem archive with llvm-ar instead of libtool.
setlocal EnableDelayedExpansion
pushd %~dp0

if not exist build mkdir build

rem raylib — third-party code builds without -Werror or warnings (-w)
set "RAYLIB_FLAGS=-std=gnu99 -O2 -w -DPLATFORM_DESKTOP -Iraylib/src -Iraylib/src/external/glfw/include"
set "OBJS="
for %%f in (rcore rshapes rtextures rtext rmodels raudio rglfw) do (
    echo   raylib/src/%%f.c
    clang !RAYLIB_FLAGS! -c raylib\src\%%f.c -o build\%%f.o
    if errorlevel 1 goto :fail
    set "OBJS=!OBJS! build\%%f.o"
)

rem clay — header-only; instantiate the implementation once
clang -std=c99 -O2 -c clay_impl.c -o build\clay_impl.o
if errorlevel 1 goto :fail
set "OBJS=!OBJS! build\clay_impl.o"

rem merge everything into the one archive
if exist third_party.a del third_party.a
llvm-ar rcs third_party.a !OBJS!
if errorlevel 1 goto :fail

echo ok: third_party/third_party.a
popd
exit /b 0

:fail
popd
exit /b 1
