#include <windows.h>

static volatile LONG g_attached = 0;

extern "C" __declspec(dllexport) int j2d_lifecycle_attached() {
    return static_cast<int>(g_attached);
}

extern "C" __declspec(dllexport) int WINAPI JNI_OnLoad(void*, void*) {
    return 0x00010008; // JNI_VERSION_1_8
}

// Deliberately bypasses the CRT. This fixture tests only the mapper's PE entry
// call contract; arbitrary CRT-dependent third-party DLLs are not enabled yet.
extern "C" BOOL WINAPI FixtureEntry(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) InterlockedExchange(&g_attached, 1);
    return TRUE;
}

#pragma comment(linker, "/ENTRY:FixtureEntry")
