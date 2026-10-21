#pragma once

#include <functional>
#include <vector>

// One tab of the main window = one kind of scan.
enum ScanKind
{
    KindSoftware,       // Uninstall registry keys (64-bit, 32-bit, per-user)
    KindDrivers,        // SetupAPI device enumeration + current driver properties
    KindWinget,         // "winget upgrade" - newer versions of installed apps
    KindWindowsUpdate,  // Windows Update Agent search - pending software and driver updates
    KindCount
};

struct ListColumn
{
    CString title;
    int     width;          // in 96-DPI pixels
};

struct ScanTable
{
    std::vector<ListColumn> columns;
    std::vector<std::vector<CString>> rows;
    CString status;
    CString notice;     // when set, shown in a message box once the job finishes
};

// Column indexes the dialog's action buttons read from.
enum
{
    DriverCol_Provider = 4,
    DriverCol_Status = 6,
    DriverCol_HardwareId = 7
};

// How bad a device's Status column is; drives the row colors on the Devices tab.
enum DeviceSeverity
{
    SeverityNone,
    SeverityDisabled,
    SeverityProblem
};

DeviceSeverity SeverityOfStatus(const CString& status);
enum
{
    WingetCol_Name = 0,
    WingetCol_Id = 1
};

enum
{
    WuCol_Title = 0,
    WuCol_UpdateId = 8
};

using ProgressFn = std::function<void(const CString&)>;
using ConfirmFn = std::function<bool(const CString&)>;   // asks the user a yes/no question

std::vector<ListColumn> ColumnsFor(ScanKind kind);

// Runs the scan synchronously; call it from a worker thread.
ScanTable RunScan(ScanKind kind);

// Creates a System Restore point, downloads and installs the given Windows Update IDs
// through the Windows Update Agent, then searches again. Needs an elevated process.
// Call it from a worker thread.
ScanTable InstallWindowsUpdates(const std::vector<CString>& updateIds, const ProgressFn& progress,
    const ConfirmFn& confirm);

bool IsGuidString(const CString& text);
