// inject.exe - loads mod DLLs into RIP3.exe (32-bit, so build as x86).
// Usage: inject.exe [--wait | --pid <pid>] <dll> [<dll> ...]
//   --wait:     wait for RIP3.exe to start (launch it from Steam) and inject right away.
//   --pid <pid>: inject into an already known process. Needed when process enumeration
//                is restricted (some sandboxes/EDR hide processes from a snapshot).
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DWORD find_game(void) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = { sizeof pe };
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
        if (!_stricmp(pe.szExeFile, "RIP3.exe")) { pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

static int inject(HANDLE p, DWORD pid, const char *arg) {
    char dll[MAX_PATH];
    if (!GetFullPathNameA(arg, sizeof dll, dll, NULL)) return 1;
    size_t n = strlen(dll) + 1;
    void *mem = VirtualAllocEx(p, NULL, n, MEM_COMMIT, PAGE_READWRITE);
    if (!mem) { printf("VirtualAllocEx failed: %lu\n", GetLastError()); return 4; }
    if (!WriteProcessMemory(p, mem, dll, n, NULL)) {
        printf("WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(p, mem, 0, MEM_RELEASE);
        return 4;
    }
    FARPROC ll = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE t = CreateRemoteThread(p, NULL, 0, (LPTHREAD_START_ROUTINE)ll, mem, 0, NULL);
    if (!t) {
        printf("CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(p, mem, 0, MEM_RELEASE);
        return 4;
    }
    WaitForSingleObject(t, 10000);
    DWORD mod = 0;
    GetExitCodeThread(t, &mod);
    CloseHandle(t);
    VirtualFreeEx(p, mem, 0, MEM_RELEASE);
    printf(mod ? "%s: injected into pid %lu\n" : "%s: LoadLibrary failed in pid %lu\n", dll, pid);
    return mod ? 0 : 5;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int wait = 0, first = 1;
    DWORD pid = 0;
    if (argc > 2 && !strcmp(argv[1], "--wait")) { wait = 1; first = 2; }
    else if (argc > 3 && !strcmp(argv[1], "--pid")) { pid = (DWORD)strtoul(argv[2], NULL, 10); first = 3; }
    if (argc <= first) { printf("usage: inject [--wait | --pid <pid>] <dll> [<dll> ...]\n"); return 1; }

    if (!pid) pid = find_game();
    if (wait && pid) {
        /* an already running game may be a stale session; the launcher checks it */
        printf("RIP3.exe is already running - close it first\n");
        return 2;
    }
    if (wait) {
        printf("waiting for RIP3.exe - launch the game from Steam...\n");
        for (DWORD t0 = GetTickCount(); !pid; pid = find_game()) {
            if (GetTickCount() - t0 > 600000) { printf("timed out\n"); return 2; }
            Sleep(5);
        }
        printf("RIP3.exe found: pid %lu\n", pid);
    }
    if (!pid) { printf("RIP3.exe is not running - launch it from Steam first\n"); return 2; }

    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                           PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!p) { printf("OpenProcess failed: %lu\n", GetLastError()); return 3; }
    int rc = 0;
    for (int i = first; i < argc; i++) rc |= inject(p, pid, argv[i]);
    CloseHandle(p);
    return rc;
}
