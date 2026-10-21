# Update Inspector

A Windows desktop app (C++ / MFC) that shows what is installed on a PC, which drivers each device uses, and which software and driver updates are available, and can install those updates.

It is also a working example of how third-party updaters find and update software and drivers on Windows, using the same APIs they use.

## Features

The window has four tabs. Every scan runs in the background, so the window stays usable while a scan runs (the cursor shows an hourglass meanwhile). Click a column header to sort, and use **Export CSV...** to save any tab.

| Tab | What it shows | How it gets the data |
|---|---|---|
| **Installed Software** | Installed programs with version, publisher, install date, MSI or EXE installer, and machine or per-user scope | `HKLM` (64-bit and 32-bit views) and `HKCU` `...\CurrentVersion\Uninstall` keys, filtered the way Windows' Apps list does |
| **Devices and Drivers** | Every present device with its driver version, date, provider, INF file, status and hardware ID. Devices that are not working are shown in red, disabled ones in amber | SetupAPI (`SetupDiGetClassDevs`, `SetupDiGetDevicePropertyW`) and `CM_Get_DevNode_Status` |
| **App Updates (winget)** | Installed apps that have a newer version | Runs `winget upgrade` and reads its output table |
| **Windows Update** | Pending software and driver updates, with KB number, size, and the driver's class, provider, date and hardware ID | Windows Update Agent API (`IUpdateSession` → `IUpdateSearcher::Search("IsInstalled=0 and IsHidden=0")`) |

Actions:

- **Search Update Catalog** (Devices tab) opens the [Microsoft Update Catalog](https://www.catalog.update.microsoft.com) searched by the device's vendor and device ID. Virtual devices created by software (for example a VPN adapter) are not in the catalog, so the app explains that instead.
- **Upgrade Checked** (winget tab) runs `winget upgrade --id <Id>` for each ticked app in a visible console window.
- **Install Checked** (Windows Update tab) downloads and installs the ticked updates through the Windows Update Agent:
  1. It asks for administrator permission (UAC) and continues in an elevated copy of the app.
  2. It creates a **System Restore point** before installing anything. If System Protection is turned off, it asks whether to continue without one.
  3. It reports the result of each update and whether a restart is needed, then refreshes the list.

  To undo an install, run `rstrui.exe` and pick *"Update Inspector - before installing Windows updates"*, or roll back a single driver in Device Manager. Windows creates at most one restore point every 24 hours by default, so a second install on the same day may rely on an earlier point.

The update tabs have checkboxes, with a "check all" box in the column header. If nothing is ticked, the buttons use the selected rows.

## Requirements

- Windows 10 version 1607 or later, or Windows 11 (x64).
- To build: Visual Studio 2022 (or later) with:
  - the **Desktop development with C++** workload
  - the **C++ ATL/MFC for latest build tools (x86 & x64)** component
- `winget` (App Installer from the Microsoft Store) for the winget tab.

## Building

Open `UpdateInspector.sln` in Visual Studio and build the **x64** configuration, or from a Developer Command Prompt:

```bat
msbuild UpdateInspector.sln /p:Configuration=Release /p:Platform=x64 -warnaserror
```

The app is written to `bin\Release\UpdateInspector.exe` (or `bin\Debug\`). MFC and the C runtime are linked statically, so the .exe runs on its own.

The project builds without warnings at warning level 4. Compiler and linker warnings are treated as errors (`/W4 /WX`), so a new warning fails the build instead of piling up.

## Continuous integration

[`.github/workflows/build.yml`](.github/workflows/build.yml) builds Debug and Release on every push to `main`, every pull request, and on demand, on a `windows-2022` GitHub runner. It runs MSBuild with `-warnaserror`, so a green run means zero warnings. The Release `UpdateInspector.exe` is uploaded as the `UpdateInspector-x64` build artifact.

## Project layout

```
UpdateInspector.sln
UpdateInspector/
  UpdateInspector.vcxproj
  UpdateInspector.cpp     App startup; handles the /install-wu command line of the elevated copy
  MainDlg.cpp/.h          Main window: tabs, list, checkboxes, colors, buttons, background jobs
  Scanners.cpp/.h         All scanning and installing: registry, SetupAPI, winget, Windows Update Agent, System Restore
  UpdateInspector.rc      Dialog layout, icon, version info
  resource.h, framework.h
  res/
    UpdateInspector.ico   App icon (16-256 px)
    make-icon.ps1         Script that draws the icon
bin/, obj/                Build output (not in source control)
```

To change the icon, edit `UpdateInspector/res/make-icon.ps1` and run it from the solution folder:

```bat
powershell -ExecutionPolicy Bypass -File UpdateInspector\res\make-icon.ps1
```

## Notes and limitations

- Store (MSIX) apps are not listed on the Installed Software tab, because they do not register in the Uninstall keys.
- Scanning needs no administrator rights. Only **Install Checked** asks for them, and winget installers may ask on their own.
- The winget tab reads `winget`'s text output, which is laid out for display. Very long names or Ids can be cut short by winget; such packages are skipped when upgrading, and the app says so.
- **Install Checked** accepts any license terms (EULAs) of the chosen updates. The confirmation message says so before anything is installed.
