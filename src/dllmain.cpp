#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dbghelp.h>
#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dbghelp.lib")

// ============================================================================
// 1. VERSION.DLL FORWARDING EXPORTS (MSVC LINKER DIRECTIVES)
// ============================================================================
#pragma comment(linker, "/export:GetFileVersionInfoA=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.GetFileVersionInfoA,@1")
#pragma comment(linker, "/export:GetFileVersionInfoByHandle=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.GetFileVersionInfoByHandle,@2")
#pragma comment(linker, "/export:GetFileVersionInfoExA=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.GetFileVersionInfoExA,@3")
#pragma comment(linker, "/export:GetFileVersionInfoExW=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.GetFileVersionInfoExW,@4")
#pragma comment(linker, "/export:GetFileVersionInfoSizeA=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.GetFileVersionInfoSizeA,@5")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExA=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.GetFileVersionInfoSizeExA,@6")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExW=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.GetFileVersionInfoSizeExW,@7")
#pragma comment(linker, "/export:GetFileVersionInfoSizeW=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.GetFileVersionInfoSizeW,@8")
#pragma comment(linker, "/export:GetFileVersionInfoW=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.GetFileVersionInfoW,@9")
#pragma comment(linker, "/export:VerFindFileA=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.VerFindFileA,@10")
#pragma comment(linker, "/export:VerFindFileW=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.VerFindFileW,@11")
#pragma comment(linker, "/export:VerInstallFileA=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.VerInstallFileA,@12")
#pragma comment(linker, "/export:VerInstallFileW=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.VerInstallFileW,@13")
#pragma comment(linker, "/export:VerLanguageNameA=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.VerLanguageNameA,@14")
#pragma comment(linker, "/export:VerLanguageNameW=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.VerLanguageNameW,@15")
#pragma comment(linker, "/export:VerQueryValueA=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.VerQueryValueA,@16")
#pragma comment(linker, "/export:VerQueryValueW=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\version.dll.VerQueryValueW,@17")

// ============================================================================
// 2. CONFIGURATION & LOGGING
// ============================================================================
struct AutoTdpSettings {
    int minWatts = 18;
    int maxWatts = 85;
    int targetGpuLoadHigh = 85;
    int targetGpuLoadLow = 65;
    int stepUpWatts = 6;
    int stepDownWatts = 2;
    int pollIntervalMs = 1000;
    bool enableConsole = true;
    uintptr_t calibratedRva = 0;
    DWORD calibratedIoctl = 0;
    int wattOffset = -1;
    bool wattsInMilliwatts = false;
    DWORD bufferSize = 0;
    std::string bufferTemplateHex;
};

static AutoTdpSettings g_Settings;
static char g_IniPath[MAX_PATH] = { 0 };
static std::atomic<bool> g_Running{ true };
static std::atomic<bool> g_AutoTdpEnabled{ true };
static std::atomic<int>  g_CurrentWatts{ 30 };
static FILE* g_LogFile = nullptr;
static CRITICAL_SECTION g_LogLock;

static void LogMsg(const char* format, ...) {
    EnterCriticalSection(&g_LogLock);
    va_list args;
    va_start(args, format);

    if (g_Settings.enableConsole) {
        vprintf(format, args);
        fflush(stdout);
    }
    if (g_LogFile) {
        vfprintf(g_LogFile, format, args);
        fflush(g_LogFile);
    }

    va_end(args);
    LeaveCriticalSection(&g_LogLock);
}

// Forward declarations
static HANDLE  g_CapturedDevice = INVALID_HANDLE_VALUE;
static DWORD   g_CapturedIoctl = 0;
static DWORD   g_CapturedInSize = 0;
static BYTE    g_LastInBuffer[512] = { 0 };
static int     g_WattOffsetInBuffer = -1;
static bool    g_WattsInMilliwatts = false;

static void SaveCalibrationToConfig() {
    if (g_IniPath[0] == '\0') return;
    char buf[128];
    snprintf(buf, sizeof(buf), "0x%08X", g_CapturedIoctl);
    WritePrivateProfileStringA("AutoTDP", "CalibratedIoctl", buf, g_IniPath);

    snprintf(buf, sizeof(buf), "%d", g_WattOffsetInBuffer);
    WritePrivateProfileStringA("AutoTDP", "WattOffset", buf, g_IniPath);

    snprintf(buf, sizeof(buf), "%d", g_WattsInMilliwatts ? 1 : 0);
    WritePrivateProfileStringA("AutoTDP", "WattsInMilliwatts", buf, g_IniPath);

    snprintf(buf, sizeof(buf), "%lu", g_CapturedInSize);
    WritePrivateProfileStringA("AutoTDP", "BufferSize", buf, g_IniPath);

    // Save buffer template in hex
    std::string hexStr;
    for (DWORD i = 0; i < g_CapturedInSize; ++i) {
        char b[4];
        snprintf(b, sizeof(b), "%02X", g_LastInBuffer[i]);
        hexStr += b;
    }
    WritePrivateProfileStringA("AutoTDP", "BufferTemplateHex", hexStr.c_str(), g_IniPath);

    LogMsg("\n>>> [AutoTDP] Calibration permanently saved to %s!\n", g_IniPath);
    LogMsg(">>> You do NOT need to calibrate or touch the slider on future restarts.\n\n");
}

static void LoadOrCreateConfig(const char* iniPath) {
    strncpy(g_IniPath, iniPath, sizeof(g_IniPath) - 1);
    if (GetFileAttributesA(iniPath) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = fopen(iniPath, "w");
        if (f) {
            fprintf(f, "[AutoTDP]\n");
            fprintf(f, "MinWatts=18\n");
            fprintf(f, "MaxWatts=85\n");
            fprintf(f, "TargetGpuLoadHigh=85\n");
            fprintf(f, "TargetGpuLoadLow=65\n");
            fprintf(f, "StepUpWatts=6\n");
            fprintf(f, "StepDownWatts=2\n");
            fprintf(f, "PollIntervalMs=1000\n");
            fprintf(f, "EnableConsole=1\n");
            fprintf(f, "CalibratedRva=0x0\n");
            fprintf(f, "CalibratedIoctl=0x0\n");
            fprintf(f, "WattOffset=-1\n");
            fprintf(f, "WattsInMilliwatts=0\n");
            fprintf(f, "BufferSize=0\n");
            fprintf(f, "BufferTemplateHex=\n");
            fclose(f);
        }
    }

    g_Settings.minWatts = GetPrivateProfileIntA("AutoTDP", "MinWatts", 18, iniPath);
    g_Settings.maxWatts = GetPrivateProfileIntA("AutoTDP", "MaxWatts", 85, iniPath);
    g_Settings.targetGpuLoadHigh = GetPrivateProfileIntA("AutoTDP", "TargetGpuLoadHigh", 85, iniPath);
    g_Settings.targetGpuLoadLow = GetPrivateProfileIntA("AutoTDP", "TargetGpuLoadLow", 65, iniPath);
    g_Settings.stepUpWatts = GetPrivateProfileIntA("AutoTDP", "StepUpWatts", 6, iniPath);
    g_Settings.stepDownWatts = GetPrivateProfileIntA("AutoTDP", "StepDownWatts", 2, iniPath);
    g_Settings.pollIntervalMs = GetPrivateProfileIntA("AutoTDP", "PollIntervalMs", 1000, iniPath);
    g_Settings.enableConsole = (GetPrivateProfileIntA("AutoTDP", "EnableConsole", 1, iniPath) != 0);

    char rvaBuf[64] = { 0 };
    GetPrivateProfileStringA("AutoTDP", "CalibratedRva", "0x0", rvaBuf, sizeof(rvaBuf), iniPath);
    g_Settings.calibratedRva = strtoull(rvaBuf, nullptr, 16);

    char ioctlBuf[64] = { 0 };
    GetPrivateProfileStringA("AutoTDP", "CalibratedIoctl", "0x0", ioctlBuf, sizeof(ioctlBuf), iniPath);
    g_Settings.calibratedIoctl = strtoul(ioctlBuf, nullptr, 16);

    g_Settings.wattOffset = GetPrivateProfileIntA("AutoTDP", "WattOffset", -1, iniPath);
    g_Settings.wattsInMilliwatts = (GetPrivateProfileIntA("AutoTDP", "WattsInMilliwatts", 0, iniPath) != 0);
    g_Settings.bufferSize = GetPrivateProfileIntA("AutoTDP", "BufferSize", 0, iniPath);

    char hexTemplateBuf[1024] = { 0 };
    GetPrivateProfileStringA("AutoTDP", "BufferTemplateHex", "", hexTemplateBuf, sizeof(hexTemplateBuf), iniPath);
    g_Settings.bufferTemplateHex = hexTemplateBuf;

    // If already calibrated previously, restore state immediately
    if (g_Settings.calibratedIoctl != 0 && g_Settings.wattOffset >= 0 && g_Settings.bufferSize > 0) {
        g_CapturedIoctl = g_Settings.calibratedIoctl;
        g_WattOffsetInBuffer = g_Settings.wattOffset;
        g_WattsInMilliwatts = g_Settings.wattsInMilliwatts;
        g_CapturedInSize = min(g_Settings.bufferSize, (DWORD)sizeof(g_LastInBuffer));

        // Parse hex template
        for (DWORD i = 0; i < g_CapturedInSize && (i * 2 + 1) < g_Settings.bufferTemplateHex.size(); ++i) {
            char byteHex[3] = { g_Settings.bufferTemplateHex[i * 2], g_Settings.bufferTemplateHex[i * 2 + 1], 0 };
            g_LastInBuffer[i] = (BYTE)strtoul(byteHex, nullptr, 16);
        }

        LogMsg("[AutoTDP] Restored previous calibration: IOCTL=0x%08X, Offset=%d, mW=%d, Size=%lu bytes.\n",
            g_CapturedIoctl, g_WattOffsetInBuffer, g_WattsInMilliwatts ? 1 : 0, g_CapturedInSize);
        LogMsg("[AutoTDP] Waiting for driver handle at startup (automatic)...\n");
    }
}

// ============================================================================
// 3. DEVICEIOCONTROL HOOK & TRACER
// ============================================================================
typedef BOOL(WINAPI* pfnDeviceIoControl)(
    HANDLE       hDevice,
    DWORD        dwIoControlCode,
    LPVOID       lpInBuffer,
    DWORD        nInBufferSize,
    LPVOID       lpOutBuffer,
    DWORD        nOutBufferSize,
    LPDWORD      lpBytesReturned,
    LPOVERLAPPED lpOverlapped
);

static pfnDeviceIoControl OriginalDeviceIoControl = nullptr;

// Function pointer for internal OneXConsole TDP setter if resolved via RVA
typedef bool(__fastcall* fnSetTDP)(int watts);
static fnSetTDP g_SetOneXConsoleTDP = nullptr;

BOOL WINAPI DetourDeviceIoControl(
    HANDLE       hDevice,
    DWORD        dwIoControlCode,
    LPVOID       lpInBuffer,
    DWORD        nInBufferSize,
    LPVOID       lpOutBuffer,
    DWORD        nOutBufferSize,
    LPDWORD      lpBytesReturned,
    LPOVERLAPPED lpOverlapped
) {
    // Call stack capture (up to 8 frames)
    void* callers[8] = { nullptr };
    USHORT frames = RtlCaptureStackBackTrace(1, 8, callers, nullptr);

    char callerInfo[512] = { 0 };
    bool isFromLocalModule = false;

    for (USHORT i = 0; i < frames; ++i) {
        HMODULE hMod = nullptr;
        if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)callers[i], &hMod) && hMod != nullptr)
        {
            char modPath[MAX_PATH] = { 0 };
            GetModuleFileNameA(hMod, modPath, MAX_PATH);
            const char* modName = strrchr(modPath, '\\');
            modName = modName ? modName + 1 : modPath;

            // Check if caller is from OneXConsole or local companion DLLs
            if (_stricmp(modName, "KERNELBASE.dll") != 0 &&
                _stricmp(modName, "KERNEL32.DLL") != 0 &&
                _stricmp(modName, "version.dll") != 0)
            {
                uintptr_t rva = (uintptr_t)callers[i] - (uintptr_t)hMod;
                char frameBuf[128];
                snprintf(frameBuf, sizeof(frameBuf), "%s+0x%llX", modName, (unsigned long long)rva);
                if (callerInfo[0] != '\0') strncat(callerInfo, " -> ", sizeof(callerInfo) - strlen(callerInfo) - 1);
                strncat(callerInfo, frameBuf, sizeof(callerInfo) - strlen(callerInfo) - 1);
                isFromLocalModule = true;
            }
        }
    }

    // If call originated from OneXConsole or helper DLL:
    if (isFromLocalModule) {
        // Automatically latch onto the driver handle from the very first call
        if (g_CapturedDevice == INVALID_HANDLE_VALUE && hDevice != INVALID_HANDLE_VALUE) {
            g_CapturedDevice = hDevice;
            LogMsg("[AutoTDP] Driver device handle captured automatically: 0x%p\n", hDevice);
        }

        if (lpInBuffer && nInBufferSize > 0) {
            // Hex representation of input buffer
            char hexDump[256] = { 0 };
            DWORD dumpLen = min(nInBufferSize, 32UL);
            for (DWORD j = 0; j < dumpLen; ++j) {
                char byteStr[8];
                snprintf(byteStr, sizeof(byteStr), "%02X ", ((BYTE*)lpInBuffer)[j]);
                strncat(hexDump, byteStr, sizeof(hexDump) - strlen(hexDump) - 1);
            }

            LogMsg("[DeviceIoControl] Caller: %s | IOCTL: 0x%08X | InSize: %lu bytes | Hex: %s\n",
                callerInfo, dwIoControlCode, nInBufferSize, hexDump);

            // Check if input buffer contains 4-byte integers that match a typical TDP range (10W-100W or 10000mW-100000mW)
            for (DWORD offset = 0; offset + 4 <= nInBufferSize; offset += 4) {
                int val = *(int*)((BYTE*)lpInBuffer + offset);
                bool detected = false;
                if (val >= 10 && val <= 100) {
                    g_WattOffsetInBuffer = (int)offset;
                    g_WattsInMilliwatts = false;
                    LogMsg(">>> [AutoTDP Calibration] Detected TDP value %dW at buffer offset %d!\n", val, offset);
                    detected = true;
                } else if (val >= 10000 && val <= 100000 && (val % 1000 == 0)) {
                    g_WattOffsetInBuffer = (int)offset;
                    g_WattsInMilliwatts = true;
                    LogMsg(">>> [AutoTDP Calibration] Detected TDP value %dmW (%dW) at buffer offset %d!\n", val, val / 1000, offset);
                    detected = true;
                }

                if (detected) {
                    g_CapturedDevice = hDevice;
                    g_CapturedIoctl = dwIoControlCode;
                    g_CapturedInSize = min(nInBufferSize, (DWORD)sizeof(g_LastInBuffer));
                    memcpy(g_LastInBuffer, lpInBuffer, g_CapturedInSize);
                    SaveCalibrationToConfig();
                    break;
                }
            }
        }
    }

    return OriginalDeviceIoControl(
        hDevice,
        dwIoControlCode,
        lpInBuffer,
        nInBufferSize,
        lpOutBuffer,
        nOutBufferSize,
        lpBytesReturned,
        lpOverlapped
    );
}

// ============================================================================
// 4. APPLY TDP (VIA REPLAY IOCTL OR INTERNAL RVA FUNCTION)
// ============================================================================
static bool ApplyTdp(int watts) {
    // Method 1: Internal RVA function pointer if resolved
    if (g_SetOneXConsoleTDP) {
        LogMsg("[AutoTDP] Invoking internal SetTDP(%dW)...\n", watts);
        return g_SetOneXConsoleTDP(watts);
    }

    // Method 2: Replay captured DeviceIoControl packet with updated watts
    if (g_CapturedDevice != INVALID_HANDLE_VALUE && g_CapturedIoctl != 0 && g_WattOffsetInBuffer >= 0) {
        BYTE sendBuf[512];
        memcpy(sendBuf, g_LastInBuffer, g_CapturedInSize);

        int targetVal = g_WattsInMilliwatts ? (watts * 1000) : watts;
        *(int*)(sendBuf + g_WattOffsetInBuffer) = targetVal;

        DWORD bytesReturned = 0;
        BOOL ok = OriginalDeviceIoControl(
            g_CapturedDevice,
            g_CapturedIoctl,
            sendBuf,
            g_CapturedInSize,
            nullptr,
            0,
            &bytesReturned,
            nullptr
        );

        LogMsg("[AutoTDP] Sent DeviceIoControl IOCTL: 0x%08X, %dW -> Result: %s\n",
            g_CapturedIoctl, watts, ok ? "SUCCESS" : "FAILED");
        return ok != FALSE;
    }

    return false;
}

// ============================================================================
// 5. GPU TELEMETRY (WINDOWS PDH 3D ENGINE READER)
// ============================================================================
class GpuTelemetry {
    PDH_HQUERY   hQuery = nullptr;
    PDH_HCOUNTER hCounter = nullptr;
    bool         initialized = false;
public:
    bool Init() {
        if (PdhOpenQueryW(nullptr, 0, &hQuery) != ERROR_SUCCESS) return false;
        // Sums 3D engine utilization across AMD Strix Halo GPU engines
        PDH_STATUS status = PdhAddEnglishCounterW(hQuery, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &hCounter);
        if (status != ERROR_SUCCESS) {
            PdhCloseQuery(hQuery);
            hQuery = nullptr;
            return false;
        }
        PdhCollectQueryData(hQuery);
        initialized = true;
        return true;
    }

    double Get3DLoad() {
        if (!initialized || !hQuery) return 0.0;
        if (PdhCollectQueryData(hQuery) != ERROR_SUCCESS) return 0.0;

        DWORD bufferSize = 0;
        DWORD itemCount = 0;
        PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, nullptr);
        if (bufferSize == 0) return 0.0;

        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(new BYTE[bufferSize]);
        double totalLoad = 0.0;

        if (PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items) == ERROR_SUCCESS) {
            for (DWORD i = 0; i < itemCount; ++i) {
                if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA) {
                    totalLoad += items[i].FmtValue.doubleValue;
                }
            }
        }
        delete[] reinterpret_cast<BYTE*>(items);
        return (totalLoad > 100.0) ? 100.0 : totalLoad;
    }

    ~GpuTelemetry() {
        if (hQuery) PdhCloseQuery(hQuery);
    }
};

// ============================================================================
// 6. AUTO-TDP BACKGROUND WORKER THREAD
// ============================================================================
DWORD WINAPI AutoTdpWorkerThread(LPVOID) {
    // Allow host process and Virbox-protected sections to unpack in memory
    Sleep(4000);

    // Get executable directory for config and logs
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    char iniPath[MAX_PATH];
    snprintf(iniPath, sizeof(iniPath), "%sautotdp.ini", exePath);
    LoadOrCreateConfig(iniPath);

    char logPath[MAX_PATH];
    snprintf(logPath, sizeof(logPath), "%sonex_autotdp.log", exePath);
    g_LogFile = fopen(logPath, "a");

    if (g_Settings.enableConsole) {
        AllocConsole();
        FILE* fDummy;
        freopen_s(&fDummy, "CONOUT$", "w", stdout);
        freopen_s(&fDummy, "CONOUT$", "w", stderr);
        SetConsoleTitleA("OneXPlayer APEX - Auto TDP Proxy (version.dll)");
    }

    LogMsg("================================================================\n");
    LogMsg("OneXPlayer APEX Auto TDP Proxy v1.0\n");
    LogMsg("Process PID: %lu | Config: %s\n", GetCurrentProcessId(), iniPath);
    LogMsg("TDP Range: %dW - %dW | Step: +%dW / -%dW\n",
        g_Settings.minWatts, g_Settings.maxWatts, g_Settings.stepUpWatts, g_Settings.stepDownWatts);
    LogMsg("Hotkey: [Ctrl + Shift + F12] toggles Auto-TDP On/Off\n");
    LogMsg("================================================================\n");

    // Initialize MinHook and hook DeviceIoControl
    if (MH_Initialize() == MH_OK) {
        HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
        FARPROC pDevIo = GetProcAddress(hKernel32, "DeviceIoControl");
        if (pDevIo) {
            if (MH_CreateHook(pDevIo, &DetourDeviceIoControl, reinterpret_cast<LPVOID*>(&OriginalDeviceIoControl)) == MH_OK &&
                MH_EnableHook(pDevIo) == MH_OK) {
                LogMsg("[Hook] DeviceIoControl hooked successfully via MinHook.\n");
            } else {
                LogMsg("[Hook Error] Failed to enable DeviceIoControl hook.\n");
            }
        }
    } else {
        LogMsg("[Hook Error] MinHook initialization failed.\n");
    }

    // Resolve calibrated RVA if present in config
    uintptr_t baseAddr = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    LogMsg("[OneXConsole] Base Address: 0x%llX\n", (unsigned long long)baseAddr);
    if (g_Settings.calibratedRva != 0) {
        g_SetOneXConsoleTDP = reinterpret_cast<fnSetTDP>(baseAddr + g_Settings.calibratedRva);
        LogMsg("[AutoTDP] Calibrated function pointer set: 0x%llX\n", (unsigned long long)g_SetOneXConsoleTDP);
    }

    LogMsg("\n[Action Needed] Open OneXConsole overlay and move the TDP slider once to calibrate!\n\n");

    GpuTelemetry gpu;
    if (!gpu.Init()) {
        LogMsg("[Telemetry Warning] Failed to initialize PDH GPU counter. Will retry...\n");
    }

    int lowLoadStreak = 0;

    while (g_Running) {
        // Hotkey: Ctrl + Shift + F12 toggles Auto-TDP
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
            (GetAsyncKeyState(VK_F12) & 1))
        {
            g_AutoTdpEnabled = !g_AutoTdpEnabled;
            Beep(g_AutoTdpEnabled ? 880 : 440, 150);
            LogMsg("[AutoTDP] Toggled %s\n", g_AutoTdpEnabled ? "ENABLED" : "PAUSED");
        }

        if (g_AutoTdpEnabled) {
            double load = gpu.Get3DLoad();
            int curr = g_CurrentWatts.load();
            int next = curr;

            if (load >= (double)g_Settings.targetGpuLoadHigh) {
                // High load: scale up rapidly to avoid frame drops
                next = min(g_Settings.maxWatts, curr + g_Settings.stepUpWatts);
                lowLoadStreak = 0;
            } else if (load <= (double)g_Settings.targetGpuLoadLow) {
                // Low load: wait for 3 consecutive ticks to prevent oscillation
                if (++lowLoadStreak >= 3) {
                    next = max(g_Settings.minWatts, curr - g_Settings.stepDownWatts);
                }
            } else {
                // In sweet spot (e.g. 65% - 85%): keep steady
                lowLoadStreak = 0;
            }

            if (next != curr) {
                g_CurrentWatts.store(next);
                LogMsg("[AutoTDP] GPU Load: %.1f%% -> Setting TDP to %dW\n", load, next);
                ApplyTdp(next);
            }
        }

        Sleep(g_Settings.pollIntervalMs);
    }

    return 0;
}

// ============================================================================
// 7. DLL ENTRY POINT
// ============================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_LogLock);
        DisableThreadLibraryCalls(hModule);
        HANDLE hThread = CreateThread(nullptr, 0, AutoTdpWorkerThread, hModule, 0, nullptr);
        if (hThread) CloseHandle(hThread);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        g_Running = false;
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        if (g_LogFile) {
            fclose(g_LogFile);
            g_LogFile = nullptr;
        }
        DeleteCriticalSection(&g_LogLock);
    }
    return TRUE;
}
