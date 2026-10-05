@echo off
rem Compile et lance verif_tampon_interpolation.cpp (BuildTools 18).
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 2
cl /std:c++20 /EHsc /W4 /nologo "%~dp0verif_tampon_interpolation.cpp" /Fo:"%TEMP%\verif_tampon.obj" /Fe:"%TEMP%\verif_tampon.exe" || exit /b 3
"%TEMP%\verif_tampon.exe"
