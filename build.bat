@echo off
setlocal
cd /d "%~dp0"
echo Compiling resources...
windres seekdial.rc -O coff -o seekdial_res.o || goto :err
echo Compiling SeekDial...
g++ -O2 -municode -mwindows -static -s seekdial.cpp seekdial_res.o -o SeekDial.exe -lole32 -lshell32 -luser32 -lcomctl32 -lgdi32 -ladvapi32 -luuid || goto :err
del seekdial_res.o >nul 2>&1
echo.
echo Built SeekDial.exe
exit /b 0
:err
echo.
echo Build failed.
exit /b 1
