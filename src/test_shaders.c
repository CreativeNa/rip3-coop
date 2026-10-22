// test_shaders.exe - compiles the upscaler shaders with d3dcompiler_47.dll
// exactly as rip3video.dll does, and prints errors and instruction counts.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "upscale_shaders.h"

typedef struct Blob Blob;
typedef struct {
    HRESULT(WINAPI *QueryInterface)(Blob *, REFIID, void **);
    ULONG(WINAPI *AddRef)(Blob *);
    ULONG(WINAPI *Release)(Blob *);
    void *(WINAPI *GetBufferPointer)(Blob *);
    SIZE_T(WINAPI *GetBufferSize)(Blob *);
} BlobVtbl;
struct Blob { const BlobVtbl *v; };
typedef HRESULT(WINAPI *D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT, Blob **, Blob **);

int main(void) {
    HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
    D3DCompile_t compile = m ? (D3DCompile_t)GetProcAddress(m, "D3DCompile") : NULL;
    if (!compile) { printf("d3dcompiler_47.dll not found\n"); return 1; }
    const char *src[] = { k_ps_catmull, k_ps_rcas, k_ps_sharp };
    const char *name[] = { "catmull", "rcas", "sharp" };
    int bad = 0;
    for (int i = 0; i < 3; i++) {
        Blob *code = NULL, *err = NULL;
        HRESULT hr = compile(src[i], strlen(src[i]), name[i], NULL, NULL, "main", "ps_3_0", 0, 0, &code, &err);
        printf("%-8s %s %u bytes\n", name[i], SUCCEEDED(hr) ? "OK" : "FAILED", code ? (unsigned)code->v->GetBufferSize(code) : 0);
        if (err) { printf("%s\n", (char *)err->v->GetBufferPointer(err)); err->v->Release(err); }
        if (code) code->v->Release(code);
        bad |= FAILED(hr);
    }
    return bad;
}
