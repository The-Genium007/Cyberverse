@echo off
REM Compile et execute verif_pont_staff.cpp. ASCII PUR (un .cmd sans BOM est lu en ANSI).
setlocal
if defined VCVARS goto :trouve
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" goto :trouve
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
:trouve
call "%VCVARS%" >nul
if errorlevel 1 exit /b 3
set "SORTIE=%TEMP%\verif_pont_staff"
if not exist "%SORTIE%" mkdir "%SORTIE%"
pushd "%~dp0"
cl /nologo /std:c++20 /W4 /WX /EHsc /utf-8 /I ..\src /Fe:"%SORTIE%\verif_pont_staff.exe" /Fo:"%SORTIE%\verif_pont_staff.obj" verif_pont_staff.cpp
if errorlevel 1 ( popd & exit /b 3 )
"%SORTIE%\verif_pont_staff.exe"
set CODE=%errorlevel%
popd
exit /b %CODE%
