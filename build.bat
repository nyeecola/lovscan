@echo off
rmdir build /s /q
mkdir build
cl main.cpp imgui\imgui.cpp imgui\backends\imgui_impl_glfw.cpp imgui\backends\imgui_impl_opengl3.cpp imgui\imgui_demo.cpp imgui\imgui_tables.cpp imgui\imgui_draw.cpp imgui\imgui_widgets.cpp /nologo /EHsc /Od /MD /Iimgui /I. /link opengl32.lib lib\glfw3.lib user32.lib gdi32.lib msvcrt.lib shell32.lib /SUBSYSTEM:CONSOLE /OUT:"build/main.exe"
build\main.exe
