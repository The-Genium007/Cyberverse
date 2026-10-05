@echo off
rem Construit Cyberverse.Red4Ext.dll de CE worktree (build\ninja-vcpkg deja configure).
rem Apres une edition de .h : `touch src/*.cpp` d'abord (ninja ne suit pas les en-tetes, MSVC francais).
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 2
cd /d "%~dp0.."
"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build\ninja-vcpkg
