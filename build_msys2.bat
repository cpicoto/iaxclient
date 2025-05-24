@echo off
C:\msys64\msys2_shell.cmd -mingw64 -defterm -no-start -here -c "cd lib/build; mingw32-make -j$(nproc)"
