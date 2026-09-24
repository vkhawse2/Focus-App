@echo off
REM Build Focus App with MinGW-w64 (https://www.mingw-w64.org/)
windres resources.rc -O coff -o resources.o
if errorlevel 1 exit /b 1
gcc -O2 -s -mwindows -municode -o FocusApp.exe focus_app.c resources.o -lwinmm -lshell32
if errorlevel 1 exit /b 1
echo Done: FocusApp.exe
