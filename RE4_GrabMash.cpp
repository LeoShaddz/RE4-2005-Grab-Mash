// RE4 Ultimate HD Edition (bio4.exe, Steam, PC) - holding the controller's A button during
// a grab makes the game count it as "moving the analog stick from side to side".
// NOT TESTED (I cannot run the game here). Based on the analysis of bio4.exe and, as a
// reference for function names, the PS2 ELF (PlGachaMove).
//
// How it works
//  * PlGachaMove (0x680D80 in bio4.exe) runs every frame while the analog stick mashing
//    prompt is active (grab). It increments the player's counter (cPlayer+0x568,
//    cPlayer = [0xC57054]) based on the controller's directional bits (0xC62F10 / 0xC62F14).
//  * The mod inserts a hook at the beginning of this function. If the controller's A button
//    (XInput) is held, the mod increments the counter every N calls by the same amount that
//    an analog stick movement would add. Outside of a grab, this function does not run,
//    so holding A has no effect.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

typedef unsigned char  u8;
typedef unsigned int   u32;

static const u32 FN_GACHAMOVE = 0x680D80;   // PlGachaMove
static const u32 ADDR_PLAYER  = 0xC57054;   // cPlayer*
static const u32 OFF_COUNTER  = 0x568;      // Grab counter offset in cPlayer

static int  gEnable = 1, gEvery = 3, gAmount = 1, gLog = 0, gVk = 0;
static int  gTick = 0;
static FILE* gF = NULL;
static void Log(const char* f, ...) {
    if (!gLog) return;
    if (!gF) gF = fopen("RE4_GrabMash.log", "a");
    if (!gF) return;
    va_list a; va_start(a, f); vfprintf(gF, f, a); va_end(a); fputc('\n', gF); fflush(gF);
}

// ---- Dynamically loaded XInput (no import library required) ----
struct XPAD { WORD wButtons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XSTATE { DWORD packet; XPAD pad; };
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XSTATE*);
static XInputGetState_t pXGet = NULL;
static void LoadXInput() {
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (const char* n : dlls) {
        HMODULE h = LoadLibraryA(n);
        if (h) { pXGet = (XInputGetState_t)GetProcAddress(h, "XInputGetState"); if (pXGet) { Log("XInput: %s", n); return; } }
    }
    Log("XInput not found");
}
static bool AHeld() {
    if (gVk && (GetAsyncKeyState(gVk) & 0x8000)) return true;   // Optional keyboard key (INI)
    if (!pXGet) return false;
    for (DWORD i = 0; i < 4; ++i) {
        XSTATE s; memset(&s, 0, sizeof(s));
        if (pXGet(i, &s) == ERROR_SUCCESS && (s.pad.wButtons & 0x1000)) return true; // XINPUT_GAMEPAD_A
    }
    return false;
}

// ---- Called at the beginning of PlGachaMove (every frame during a grab) ----
static void __cdecl OnGacha() {
    if (!gEnable) return;
    if (!AHeld()) { gTick = 0; return; }
    if (++gTick < gEvery) return;
    gTick = 0;
    u32 p = *(volatile u32*)ADDR_PLAYER;
    if (!p) return;
    *(volatile int*)(p + OFF_COUNTER) += gAmount;
}

static bool Match(u32 addr, const u8* sig, u32 n) {
    if (IsBadReadPtr((void*)addr, n)) return false;
    return memcmp((void*)addr, sig, n) == 0;
}

static bool Install() {
    // PlGachaMove: push edi / mov edi,[0xC57054] / push 0 / push 0 / push 0x11 / push 0xB ...
    static const u8 sig[] = { 0x57, 0x8B, 0x3D, 0x54, 0x70, 0xC5, 0x00, 0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x11, 0x6A, 0x0B };
    if (!Match(FN_GACHAMOVE, sig, sizeof(sig))) return false;

    // Trampoline: pushfd; pushad; call OnGacha; popad; popfd; [7 original bytes]; jmp FN+7
    u8* tr = (u8*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    u8* p = tr;
    *p++ = 0x9C;                // pushfd
    *p++ = 0x60;                // pushad
    *p++ = 0xE8; *(u32*)p = (u32)&OnGacha - (u32)(p + 4); p += 4;
    *p++ = 0x61;                // popad
    *p++ = 0x9D;                // popfd
    memcpy(p, (void*)FN_GACHAMOVE, 7); p += 7;
    *p++ = 0xE9; *(u32*)p = (FN_GACHAMOVE + 7) - (u32)(p + 4); p += 4;

    DWORD old;
    if (!VirtualProtect((void*)FN_GACHAMOVE, 7, PAGE_EXECUTE_READWRITE, &old)) return false;
    u8* f = (u8*)FN_GACHAMOVE;
    f[0] = 0xE9; *(u32*)(f + 1) = (u32)tr - (FN_GACHAMOVE + 5);
    f[5] = 0x90; f[6] = 0x90;
    VirtualProtect((void*)FN_GACHAMOVE, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)FN_GACHAMOVE, 7);
    return true;
}

static DWORD WINAPI Init(LPVOID) {
    char ini[MAX_PATH]; GetModuleFileNameA(NULL, ini, MAX_PATH);
    char* s = strrchr(ini, '\\'); if (s) *(s + 1) = 0;
    strcat(ini, "RE4_GrabMash.ini");
    gEnable = GetPrivateProfileIntA("Main", "Enable", 1, ini);
    gEvery  = GetPrivateProfileIntA("Main", "EveryNFrames", 3, ini);
    gAmount = GetPrivateProfileIntA("Main", "Amount", 1, ini);
    gVk     = GetPrivateProfileIntA("Main", "ExtraKeyVK", 0, ini);
    gLog    = GetPrivateProfileIntA("Main", "Log", 0, ini);
    if (gEvery < 1) gEvery = 1;
    if (!gEnable) return 0;
    LoadXInput();
    // The code may not be unpacked or available yet when the DLL loads: retry for ~60 seconds
    for (int i = 0; i < 120; ++i) {
        if (Install()) { Log("Hook installed at PlGachaMove (0x%X)", FN_GACHAMOVE); return 0; }
        Sleep(500);
    }
    Log("PlGachaMove signature mismatch: different EXE, nothing was modified");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(NULL, 0, Init, NULL, 0, NULL); }
    return TRUE;
}