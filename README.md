# OneXPlayer APEX - Auto TDP Proxy Hook (`version.dll`)

A lightweight proxy DLL injector for `OneXConsole.exe` on the **OneXPlayer APEX** (Ryzen AI Max+ 395 / Strix Halo) that adds dynamic, GPU-load-driven Auto TDP while keeping all native fan curves, RGB, and controller mapping intact.

---

## How It Works

1. **Clean DLL Proxying**: Windows applications look in their local folder for DLLs before checking `System32`. This DLL impersonates `version.dll` and forwards all 17 standard Windows version APIs directly to `C:\Windows\System32\version.dll`.
2. **MinHook `DeviceIoControl` Tracer**: Intercepts hardware driver calls issued by OneXConsole to capture the driver handle (`WinRing0` or `PawnIO`) and the exact packet structure when you move the TDP slider.
3. **PDH GPU Telemetry Loop**: Runs a background thread polling `\GPU Engine(*engtype_3D)\Utilization Percentage` every 1000ms.
   - If GPU load is $\ge 85\%$: steps TDP up aggressively (`+6W`) to prevent frame drops.
   - If GPU load sits $\le 65\%$ for 3 consecutive checks: steps TDP down gradually (`-2W`) to conserve battery and keep thermals quiet.
4. **Zero UI Disruption**: Because OneXConsole's overlay reads live package power directly from hardware telemetry registers, the native OSD automatically displays the new wattage.

---

## Project Structure

```
onex-auto-tdp/
├── CMakeLists.txt         # CMake build configuration
├── version.def            # DLL export forwarding definitions
├── autotdp.ini            # Runtime configuration file
├── build.bat              # 1-click Windows compilation script
├── src/
│   └── dllmain.cpp        # Proxy forwards, MinHook detour, PDH loop
└── vendor/
    └── minhook/           # Vendored MinHook submodule
```

---

## 1. Building on Windows

### Prerequisites
- Visual Studio 2022 / 2019 (with Desktop Development with C++) OR Build Tools for Visual Studio
- CMake 3.15+ (included with Visual Studio or via winget: `winget install Kitware.CMake`)

### Build Steps
1. Copy or clone the `onex-auto-tdp` folder onto your OneXPlayer APEX (or a Windows dev PC).
2. Double-click `build.bat` (or open Developer Command Prompt and run `build.bat`).
3. The compiled binary will be placed in `dist\version.dll`.

*(Alternatively, open the folder in Visual Studio as a CMake project and select **Build > Build All**).*

---

## 2. Installation & Calibration

1. Exit **OneXConsole** completely (check system tray and Task Manager).
2. Copy `dist\version.dll` and `autotdp.ini` into your OneXConsole directory:
   - Typical path: `C:\Program Files\OneXConsole\` or `C:\Program Files (x86)\OneXConsole\`
3. Start **OneXConsole.exe**.
4. A console window titled **`OneXPlayer APEX - Auto TDP Proxy (version.dll)`** will appear:
   - You will see MinHook initialize and hook `DeviceIoControl`.
5. **Calibrate (One-Time Only)**:
   - Open the OneXConsole overlay menu and drag the TDP slider once (e.g. from 30W to 35W).
   - The console window will log:
     ```
     [DeviceIoControl] Caller: OneXConsole.exe+0xXXXXX | IOCTL: 0x9C40208C | InSize: 12 bytes
     >>> [AutoTDP Calibration] Detected TDP value 35W at buffer offset 4!
     >>> [AutoTDP] Calibration permanently saved to autotdp.ini!
     ```
   - **On future restarts, zero action is needed.** The driver handle is automatically grabbed on boot and the saved calibration activates immediately.
   - Once verified, you can set `EnableConsole=0` in `autotdp.ini` to run completely silent in the background.

---

## 3. Configuration & Hotkeys

Edit `autotdp.ini` in the OneXConsole folder to tweak behavior:

| Setting | Default | Description |
| :--- | :--- | :--- |
| `MinWatts` | `18` | Minimum power floor (light 2D games, desktop) |
| `MaxWatts` | `85` | Maximum power ceiling for Strix Halo |
| `TargetGpuLoadHigh` | `85` | Scale up wattage when 3D load exceeds this % |
| `TargetGpuLoadLow` | `65` | Scale down wattage when 3D load falls below this % |
| `StepUpWatts` | `6` | Wattage increment on high load spikes |
| `StepDownWatts` | `2` | Wattage decrement on sustained low load |
| `PollIntervalMs` | `1000` | Telemetry loop update rate in ms |
| `EnableConsole` | `1` | Show debug console (`1` during setup, `0` to run silently in background) |

### Global Hotkey
- **`Ctrl + Shift + F12`**: Toggles Auto-TDP on / off.
  - High beep ($880\text{ Hz}$): Auto TDP Enabled
  - Low beep ($440\text{ Hz}$): Auto TDP Paused (reverts to static profile)

---

## 4. Uninstallation / Rollback

To uninstall or test without the proxy:
1. Exit OneXConsole.
2. Delete or rename `version.dll` inside the OneXConsole folder.
3. Windows will immediately revert to loading the native system library at `C:\Windows\System32\version.dll`.
