// inject.exe - loads mod DLLs into RIP3.exe (32-bit, so build as x86).
// Usage: inject.exe [--wait] <dll> [<dll> ...]
//   --wait: wait for RIP3.exe to start (launch it from Steam) and inject right away.
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
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
    WriteProcessMemory(p, mem, dll, n, NULL);
    FARPROC ll = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE t = CreateRemoteThread(p, NULL, 0, (LPTHREAD_START_ROUTINE)ll, mem, 0, NULL);
    if (!t) { printf("CreateRemoteThread failed: %lu\n", GetLastError()); return 4; }
    WaitForSingleObject(t, 10000);
    DWORD mod = 0;
    GetExitCodeThread(t, &mod);
    CloseHandle(t);
    VirtualFreeEx(p, mem, 0, MEM_RELEASE);
    printf(mod ? "%s: injected into pid %lu\n" : "%s: LoadLibrary failed in pid %lu\n", dll, pid);
    return mod ? 0 : 5;
}

int main(int argc, char **argv) {
    int wait = argc > 2 && !strcmp(argv[1], "--wait");
    int first = 1 + wait;
    if (argc <= first) { printf("usage: inject [--wait] <dll> [<dll> ...]\n"); return 1; }

    DWORD pid = find_game();
    if (wait) {
        if (pid) { printf("RIP3.exe is already running - close it first\n"); return 2; }
        printf("waiting for RIP3.exe - launch the game from Steam...\n");
        for (DWORD t0 = GetTickCount(); !pid; pid = find_game()) {
            if (GetTickCount() - t0 > 600000) { printf("timed out\n"); return 2; }
            Sleep(5);
        }
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
