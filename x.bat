@echo off
rem x.bat -- Windows counterpart of x.sh; keep the two behaviourally in
rem sync. One clang++ invocation per root binary; modules (.hpp) are included
rem by roots (.cpp); only roots are compiled.
rem
rem Usage: x.bat [command] [--release] [--debug]
rem   clean        remove build artifacts
rem   build        build all roots (default command; runs third_party first when necessary)
rem   run          build if out of date, then run the game
rem   test         build if out of date, then run the test suite
rem   third_party  build dependencies into third_party/third_party.a
rem
rem Profiles: default is -O1 with ASSERT/LOG enabled; --release is -O2 with
rem them compiled out; --debug adds -g to either.
rem
rem File-timestamp checks are delegated to embedded PowerShell one-liners
rem (batch has no portable newer-than test); everything else is plain batch.
setlocal EnableDelayedExpansion
pushd %~dp0

set CMD=build
set RELEASE=0
set DEBUG=0
for %%a in (%*) do (
    set KNOWN=0
    for %%c in (clean build run test third_party) do if "%%a"=="%%c" set "CMD=%%a" & set KNOWN=1
    if "%%a"=="--release" set RELEASE=1& set KNOWN=1
    if "%%a"=="--debug"   set DEBUG=1& set KNOWN=1
    if !KNOWN!==0 echo unknown argument: %%a& popd & exit /b 1
)

rem -D_CRT_SECURE_NO_WARNINGS: windows-only addition -- MSVC's CRT marks
rem fopen/strerror deprecated, which -Werror would otherwise turn fatal.
rem -Wno-missing-designated-field-initializers: clang 21 warning not in Apple
rem clang; partial designated init with the rest zeroed is our ZII style.
rem -Wno-reorder-init-list: out-of-order designated init is our style (ZII
rem PODs; field order at the call site follows meaning, not declaration).
set "BASE_FLAGS=-std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -Wno-error=unused-variable -Wno-missing-designated-field-initializers -Wno-reorder-init-list -D_CRT_SECURE_NO_WARNINGS -Ithird_party/raylib/src"
if !RELEASE!==1 (
    set "FLAGS=!BASE_FLAGS! -O2"
    set "PROFILE=release"
) else (
    set "FLAGS=!BASE_FLAGS! -O1 -DASSERT_ENABLE -DLOG_ENABLE"
    set "PROFILE=default"
)
if !DEBUG!==1 (
    set "FLAGS=!FLAGS! -g"
    set "PROFILE=!PROFILE!+debug"
)

set "BIN=build\imperium.exe"
set "STAMP=build\flags"

goto cmd_!CMD!

rem ---------------------------------------------------------------- commands

:cmd_clean
if exist build rmdir /s /q build
echo ok: cleaned
popd & exit /b 0

:cmd_third_party
call third_party\build.bat
set EC=!errorlevel!
popd & exit /b !EC!

:cmd_build
call :do_build
set EC=!errorlevel!
popd & exit /b !EC!

:cmd_run
call :build_needed
if !NEEDED!==1 (
    call :do_build
    if errorlevel 1 popd & exit /b 1
)
!BIN!
set EC=!errorlevel!
popd & exit /b !EC!

:cmd_test
call :build_needed
if !NEEDED!==1 (
    call :do_build
    if errorlevel 1 popd & exit /b 1
)
build\test.exe
set EC=!errorlevel!
popd & exit /b !EC!

rem ------------------------------------------------------------ subroutines

:third_party_if_needed
rem Vendored library sources are effectively frozen -- staleness only tracks
rem our own third_party files. After editing vendored code, run
rem `x.bat third_party` by hand.
powershell -NoProfile -Command "$a='third_party/third_party.a'; if(-not(Test-Path $a)){exit 0}; $t=(Get-Item $a).LastWriteTime; if((Get-Item 'third_party/build.bat').LastWriteTime -gt $t){exit 0}; exit 1"
if !errorlevel! EQU 0 (
    call third_party\build.bat
    if errorlevel 1 exit /b 1
)
exit /b 0

:build_needed
rem Sets NEEDED=1 when any root binary is missing, the flag stamp changed, or
rem sources / x.bat / third_party.a are newer than the game binary.
set NEEDED=0
if not exist !STAMP! set NEEDED=1
if !NEEDED!==0 (
    set "STAMPVAL="
    set /p STAMPVAL=<!STAMP!
    if not "!STAMPVAL!"=="!FLAGS!" set NEEDED=1
)
if !NEEDED!==0 (
    rem sources are scanned recursively: modules may live in subdirs (src/ui/...)
    powershell -NoProfile -Command "$bin='build/imperium.exe'; if(-not(Test-Path $bin)){exit 0}; foreach($r in (Get-ChildItem src -Filter *.cpp)){ if($r.Name -eq 'ray.cpp'){continue}; $n=$r.BaseName; if($n -eq 'main'){$n='imperium'}; if(-not(Test-Path ('build/'+$n+'.exe'))){exit 0} }; $bt=(Get-Item $bin).LastWriteTime; if(Get-ChildItem src -Recurse -Include *.cpp,*.hpp | Where-Object {$_.LastWriteTime -gt $bt}){exit 0}; if((Get-Item 'x.bat').LastWriteTime -gt $bt){exit 0}; if((Test-Path 'third_party/third_party.a') -and ((Get-Item 'third_party/third_party.a').LastWriteTime -gt $bt)){exit 0}; exit 1"
    if !errorlevel! EQU 0 set NEEDED=1
)
exit /b 0

:do_build
call :third_party_if_needed
if errorlevel 1 exit /b 1
if not exist build mkdir build
set "LIBS="
if exist third_party\third_party.a (
    rem the .lib imports are raylib's Windows platform dependencies (clang
    rem targets MSVC here, so libs are named .lib-style for link.exe/lld)
    set "LIBS=third_party/third_party.a -Wl,opengl32.lib,gdi32.lib,winmm.lib,user32.lib,shell32.lib"
)
clang++ !FLAGS! -c src/ray.cpp -o build/ray.o
if errorlevel 1 exit /b 1
set "OUTS="
rem Every .cpp in src/ is a root and builds to its own binary; main.cpp is the
rem game. Exception: ray.cpp is the raylib boundary TU (see src/ray.hpp) --
rem compiled once to build/ray.o and linked into every binary, never a root.
for %%f in (src\*.cpp) do (
    if /i not "%%~nxf"=="ray.cpp" (
        set "OUT=build\%%~nf.exe"
        if /i "%%~nf"=="main" set "OUT=!BIN!"
        clang++ !FLAGS! src\%%~nxf build\ray.o !LIBS! -o !OUT!
        if errorlevel 1 exit /b 1
        set "OUTS=!OUTS! !OUT!"
    )
)
>!STAMP! echo !FLAGS!
call :gen_compile_commands
echo ok:!OUTS! (!PROFILE!)
exit /b 0

:gen_compile_commands
rem compile_commands.json -- one entry per src file, always the default
rem profile, so clangd checks every module standalone against the config most
rem code runs in. -x c++ makes clangd treat module .hpp files as TUs.
set "CC_FLAGS=!BASE_FLAGS! -O1 -DASSERT_ENABLE -DLOG_ENABLE"
rem src is scanned recursively so modules in subdirs (src/ui/...) get entries.
powershell -NoProfile -Command "$flags='!CC_FLAGS!'; $dir=(Get-Location).Path -replace '\\','/'; $files=Get-ChildItem src -Recurse -Include *.cpp,*.hpp; $entries=foreach($f in $files){ $p=$f.FullName.Substring((Get-Location).Path.Length+1) -replace '\\','/'; if($p -like '*.hpp'){$cmd='clang++ -x c++ '+$flags+' -c '+$p}else{$cmd='clang++ '+$flags+' -c '+$p}; [pscustomobject]@{directory=$dir; file=$p; command=$cmd} }; [IO.File]::WriteAllText($dir+'/compile_commands.json',(ConvertTo-Json @($entries)))"
exit /b 0
