RR Studio Internal - experimental source archive

Build on Windows x64 with Visual Studio C++ tools:
  build_internal.bat
The resulting binary is build\RobRunInternal.dll. To build the launcher:
  build_injector.bat
The source includes the required ImGui/FreeType files and bundled UI assets.

The separate TEST runtime ZIP contains a DLL and injector, not a directly
runnable game EXE. Old core offsets are present in this source and current
game compatibility/stability have not been verified. Do not disable Windows
or game security protections to make it run.

No personal logs, profiles, crash dumps, prior build artifacts or compiled
symbols are included in this source ZIP.
