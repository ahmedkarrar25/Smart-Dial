@echo off
setlocal
cd /d "%~dp0"
echo Compiling resources...
windres smartdial.rc -O coff -o smartdial_res.o || goto :err
echo Compiling SmartDial...
g++ -O2 -municode -mwindows -static -s smartdial.cpp smartdial_res.o -o SmartDial.exe -lole32 -lshell32 -luser32 -lcomctl32 -lgdi32 -ladvapi32 -luuid || goto :err
del smartdial_res.o >nul 2>&1
echo.
echo Built SmartDial.exe
exit /b 0
:err
echo.
echo Build failed.
exit /b 1
