@echo off
REM Compile et execute verif_vecteurs_noyau.cpp contre vecteurs.json (depot Tessera).
REM   verif_vecteurs_noyau.cmd [chemin\vecteurs.json]
REM ASCII PUR (un .cmd sans BOM est lu en ANSI).
setlocal
if defined VCVARS goto :trouve
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" goto :trouve
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
:trouve
call "%VCVARS%" >nul
if errorlevel 1 exit /b 3
set "VEC=%~1"
if "%VEC%"=="" set "VEC=C:\tw\client-rust\tessera-core\partage\vecteurs.json"
pushd "%~dp0"
cl /nologo /std:c++20 /W4 /WX /wd4244 /EHsc /I ..\src /Fe:verif_vecteurs_noyau.exe /Fo:verif_vecteurs_noyau.obj verif_vecteurs_noyau.cpp
if errorlevel 1 ( popd & exit /b 3 )
"%~dp0verif_vecteurs_noyau.exe" "%VEC%"
set CODE=%errorlevel%
popd
exit /b %CODE%
