@echo off
rmdir build /s /q
mkdir build
cl main.cpp /nologo /EHsc /Od /MD /link opengl32.lib lib\glfw3.lib user32.lib gdi32.lib msvcrt.lib shell32.lib /SUBSYSTEM:CONSOLE /OUT:"build/main.exe"
build\main.exe
