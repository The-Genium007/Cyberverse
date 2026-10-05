@echo off
REM Compile et lance les deux verifications HORS JEU de la foule derivee :
REM   1. les 19 vecteurs croises Rust/C++ du noyau (PopulationDerivee.hpp) ;
REM   2. la logique pure des hooks (FouleDerivee.hpp).
REM Sortie 0 = tout tient. ASCII PUR (un .cmd sans BOM est lu en ANSI).
setlocal
if not defined VCVARS set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo ECHEC : vcvars64.bat introuvable. Definis VCVARS.
  exit /b 3
)
call "%VCVARS%" >nul
if not defined TEMP_FOULE set "TEMP_FOULE=%TEMP%\verif-foule"
if not exist "%TEMP_FOULE%" mkdir "%TEMP_FOULE%"
pushd "%~dp0"
cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /Fo"%TEMP_FOULE%\\" /Fe:"%TEMP_FOULE%\verif_population.exe" verif_population_derivee.cpp
if errorlevel 1 ( echo ECHEC : compilation des vecteurs. & popd & exit /b 3 )
"%TEMP_FOULE%\verif_population.exe" "%~dp0population-derivee-vecteurs.json"
if errorlevel 1 ( echo ROUGE : vecteurs. & popd & exit /b 1 )
cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /Fo"%TEMP_FOULE%\\" /Fe:"%TEMP_FOULE%\verif_foule.exe" verif_foule_derivee.cpp
if errorlevel 1 ( echo ECHEC : compilation de la logique. & popd & exit /b 3 )
"%TEMP_FOULE%\verif_foule.exe"
set CODE=%errorlevel%
popd
exit /b %CODE%
