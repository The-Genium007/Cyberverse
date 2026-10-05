@echo off
rem Compile et lance verif_ecrivain_vol.cpp (BuildTools 18). A lancer depuis n'importe ou.
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 2
cl /std:c++20 /EHsc /W4 /nologo "%~dp0verif_ecrivain_vol.cpp" /Fo:"%TEMP%\verif_vol.obj" /Fe:"%TEMP%\verif_vol.exe" || exit /b 3
"%TEMP%\verif_vol.exe"
