#!/bin/sh
# Build psrun_host.dll with mingw-w64 (macOS: brew install mingw-w64)
set -e
cd "$(dirname "$0")"

x86_64-w64-mingw32-g++ -O2 -Wall -shared -o psrun_host.dll psrun_host.cpp \
    -static-libgcc -static-libstdc++ -fno-exceptions -fno-rtti -luser32

echo "[+] psrun_host.dll built. Imports (expect kernel32/user32/api-ms-win-crt-*):"
x86_64-w64-mingw32-objdump -p psrun_host.dll | grep 'DLL Name' || true
echo
echo "next: dotnet build PSRunner.csproj -c Release   -> PSRunner.dll"
