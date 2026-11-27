/* Dedicated launcher for the in-tree mapper. Never runs as part of a build.
 * --check inspects files/process names only; --inject explicitly maps once. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include "internal/rr_map.h"

static DWORD game_pid(void) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD found = 0;
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W p = {0};
    p.dwSize = sizeof(p);
    if (Process32FirstW(snapshot, &p)) do {
        if (!_wcsicmp(p.szExeFile, L"ROB RUN.exe")) {
            if (found) { found = 0; break; } /* ambiguous; never guess */
            found = p.th32ProcessID;
        }
    } while (Process32NextW(snapshot, &p));
    CloseHandle(snapshot);
    return found;
}

int main(int argc, char** argv) {
    if (argc != 3 || (strcmp(argv[1], "--check") && strcmp(argv[1], "--inject"))) {
        fputs("Usage: RR-Injector.exe --check|--inject <absolute DLL path>\n", stderr);
        return 2;
    }
    char path[MAX_PATH];
    DWORD n = GetFullPathNameA(argv[2], sizeof(path), path, NULL);
    if (!n || n >= sizeof(path) || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "DLL not found: %s\n", argv[2]);
        return 3;
    }
    DWORD pid = game_pid();
    if (!pid) {
        fputs("ROB RUN.exe not found, or multiple copies running. Start one copy first.\n", stderr);
        return 4;
    }
    printf("Target: ROB RUN.exe (PID %lu)\nDLL: %s\n", (unsigned long)pid, path);
    if (!strcmp(argv[1], "--check")) {
        puts("CHECK_OK: no injection performed");
        return 0;
    }
    RR_MAP_RESULT result = {0};
    char error[512] = {0};
    if (!rr_map_dll(pid, path, &result, error, sizeof(error))) {
        fprintf(stderr, "MAP_FAILED: %s\n", error[0] ? error : "unknown error");
        return 5;
    }
    printf("MAP_RETURNED: base=%llX size=%X thread=%lu\n"
           "Mapping returned; this does not prove menu/features work in the game.\n",
           (unsigned long long)result.base, result.size, (unsigned long)result.thread_id);
    return 0;
}
