#include "framework.h"
#include "Scanners.h"

#include <initguid.h>   // must precede devpkey.h so the DEVPKEY_* values are defined here
#include <devpkey.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <wuapi.h>
#include <shlwapi.h>
#include <srrestoreptapi.h>

#include <algorithm>
#include <set>
#include <string>

namespace
{
    // ---------------------------------------------------------------------------
    // Shared helpers
    // ---------------------------------------------------------------------------

    CString FormatDate(const SYSTEMTIME& st)
    {
        CString text;
        text.Format(L"%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
        return text;
    }

    CString HResultText(HRESULT hr)
    {
        CString text;
        text.Format(L"0x%08X", static_cast<unsigned>(hr));

        switch (static_cast<unsigned>(hr))
        {
        case 0x8024402Cu:   // WU_E_PT_WINHTTP_NAME_NOT_RESOLVED
        case 0x80072EE2u:   // WININET_E_TIMEOUT
        case 0x80072EFDu:   // WININET_E_CANNOT_CONNECT
            text += L" - cannot reach the update server (check network or proxy).";
            return text;
        }

        LPWSTR message = nullptr;
        if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, hr, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr))
        {
            CString detail(message);
            LocalFree(message);
            text += L" - " + detail.Trim();
        }

        return text;
    }

    // ---------------------------------------------------------------------------
    // Installed software: HKLM/HKCU ...\CurrentVersion\Uninstall
    // ---------------------------------------------------------------------------

    CString RegString(CRegKey& key, LPCWSTR name)
    {
        ULONG chars = 0;
        if (key.QueryStringValue(name, nullptr, &chars) != ERROR_SUCCESS || chars == 0)
        {
            return CString();
        }

        CString value;
        LONG rc = key.QueryStringValue(name, value.GetBuffer(chars), &chars);
        value.ReleaseBuffer();
        return rc == ERROR_SUCCESS ? value : CString();
    }

    CString FormatInstallDate(const CString& raw)
    {
        // Installers usually write yyyymmdd.
        if (raw.GetLength() == 8 && raw.SpanIncluding(L"0123456789") == raw)
        {
            return raw.Left(4) + L"-" + raw.Mid(4, 2) + L"-" + raw.Mid(6, 2);
        }

        return raw;
    }

    struct UninstallSource
    {
        HKEY root;
        REGSAM view;
        LPCWSTR scope;
    };

    void ReadUninstallKey(const UninstallSource& source, std::set<CString>& seen, ScanTable& table)
    {
        CRegKey uninstall;
        if (uninstall.Open(source.root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
            KEY_READ | source.view) != ERROR_SUCCESS)
        {
            return;
        }

        for (DWORD index = 0;; ++index)
        {
            wchar_t subKey[256];
            DWORD length = _countof(subKey);
            LONG rc = uninstall.EnumKey(index, subKey, &length);
            if (rc == ERROR_NO_MORE_ITEMS)
            {
                break;
            }

            if (rc != ERROR_SUCCESS)
            {
                continue;
            }

            CRegKey app;
            if (app.Open(uninstall, subKey, KEY_READ | source.view) != ERROR_SUCCESS)
            {
                continue;
            }

            // The same filters "Apps & features" applies: no name, hidden components and child patches are skipped.
            CString name = RegString(app, L"DisplayName");
            if (name.IsEmpty())
            {
                continue;
            }

            DWORD systemComponent = 0;
            if (app.QueryDWORDValue(L"SystemComponent", systemComponent) == ERROR_SUCCESS && systemComponent)
            {
                continue;
            }

            if (!RegString(app, L"ParentKeyName").IsEmpty())
            {
                continue;
            }

            CString version = RegString(app, L"DisplayVersion");
            if (!seen.insert(name + L"|" + version).second)
            {
                continue;
            }

            DWORD windowsInstaller = 0;
            app.QueryDWORDValue(L"WindowsInstaller", windowsInstaller);

            table.rows.push_back({
                name,
                version,
                RegString(app, L"Publisher"),
                FormatInstallDate(RegString(app, L"InstallDate")),
                windowsInstaller ? L"MSI" : L"EXE",
                source.scope,
                subKey,
                });
        }
    }

    ScanTable ScanInstalledSoftware()
    {
        ScanTable table;
        table.columns = ColumnsFor(KindSoftware);

        const UninstallSource sources[] =
        {
            { HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, L"Machine (64-bit)" },
            { HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, L"Machine (32-bit)" },
            { HKEY_CURRENT_USER,  0,               L"Current user" },
        };

        std::set<CString> seen;
        for (const auto& source : sources)
        {
            ReadUninstallKey(source, seen, table);
        }

        std::sort(table.rows.begin(), table.rows.end(),
            [](const auto& a, const auto& b) { return StrCmpLogicalW(a[0], b[0]) < 0; });

        table.status.Format(L"%d programs found in the Uninstall registry keys (64-bit, 32-bit and per-user).",
            static_cast<int>(table.rows.size()));
        return table;
    }

    // ---------------------------------------------------------------------------
    // Devices and drivers: SetupAPI + Configuration Manager
    // ---------------------------------------------------------------------------

    bool GetDeviceProperty(HDEVINFO devices, SP_DEVINFO_DATA& device, const DEVPROPKEY& key,
        DEVPROPTYPE& type, std::vector<BYTE>& buffer)
    {
        DWORD size = 0;
        if (!SetupDiGetDevicePropertyW(devices, &device, &key, &type, nullptr, 0, &size, 0)
            && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            return false;
        }

        if (size == 0)
        {
            return false;
        }

        buffer.assign(size + 2 * sizeof(wchar_t), 0);  // extra zeroes keep strings terminated
        return SetupDiGetDevicePropertyW(devices, &device, &key, &type, buffer.data(), size, nullptr, 0) != FALSE;
    }

    CString DevicePropertyText(HDEVINFO devices, SP_DEVINFO_DATA& device, const DEVPROPKEY& key)
    {
        DEVPROPTYPE type = 0;
        std::vector<BYTE> buffer;
        if (!GetDeviceProperty(devices, device, key, type, buffer))
        {
            return CString();
        }

        switch (type)
        {
        case DEVPROP_TYPE_STRING:
        case DEVPROP_TYPE_STRING_LIST:  // for hardware IDs the first entry is the most specific
            return CString(reinterpret_cast<const wchar_t*>(buffer.data()));
        case DEVPROP_TYPE_FILETIME:
        {
            SYSTEMTIME st{};
            if (FileTimeToSystemTime(reinterpret_cast<const FILETIME*>(buffer.data()), &st))
            {
                return FormatDate(st);
            }
            break;
        }
        }
        return CString();
    }

    CString DeviceStatusText(DEVINST devInst)
    {
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, devInst, 0) != CR_SUCCESS)
        {
            return L"Unknown";
        }

        if (!(status & DN_HAS_PROBLEM))
        {
            return L"OK";
        }

        CString text;
        switch (problem)
        {
        case CM_PROB_FAILED_INSTALL:
            text.Format(L"No driver (code %u)", problem); break;
        case CM_PROB_DISABLED:
            text.Format(L"Disabled (code %u)", problem); break;
        case CM_PROB_FAILED_START:
            text.Format(L"Cannot start (code %u)", problem); break;
        default:
            text.Format(L"Problem (code %u)", problem); break;
        }

        return text;
    }

    ScanTable ScanDevicesAndDrivers()
    {
        ScanTable table;
        table.columns = ColumnsFor(KindDrivers);

        HDEVINFO devices = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
        if (devices == INVALID_HANDLE_VALUE)
        {
            table.status = L"SetupDiGetClassDevs failed: " + HResultText(HRESULT_FROM_WIN32(GetLastError()));
            return table;
        }

        int problems = 0, disabled = 0;
        SP_DEVINFO_DATA device{ sizeof(device) };
        for (DWORD index = 0; SetupDiEnumDeviceInfo(devices, index, &device); ++index)
        {
            CString name = DevicePropertyText(devices, device, DEVPKEY_Device_FriendlyName);
            if (name.IsEmpty())
            {
                name = DevicePropertyText(devices, device, DEVPKEY_Device_DeviceDesc);
            }

            if (name.IsEmpty())
            {
                name = DevicePropertyText(devices, device, DEVPKEY_Device_InstanceId);
            }

            CString status = DeviceStatusText(device.DevInst);
            switch (SeverityOfStatus(status))
            {
            case SeverityProblem:
                ++problems;
                break;
            case SeverityDisabled:
                ++disabled;
                break;
            default:
                break;
            }

            table.rows.push_back({
                name,
                DevicePropertyText(devices, device, DEVPKEY_Device_Class),
                DevicePropertyText(devices, device, DEVPKEY_Device_DriverVersion),
                DevicePropertyText(devices, device, DEVPKEY_Device_DriverDate),
                DevicePropertyText(devices, device, DEVPKEY_Device_DriverProvider),
                DevicePropertyText(devices, device, DEVPKEY_Device_DriverInfPath),
                status,
                DevicePropertyText(devices, device, DEVPKEY_Device_HardwareIds),
                });
        }

        SetupDiDestroyDeviceInfoList(devices);

        // Devices that are not working first, then disabled ones, then by class and name.
        std::sort(table.rows.begin(), table.rows.end(), [](const auto& a, const auto& b) {
            const auto aSeverity = SeverityOfStatus(a[DriverCol_Status]);
            const auto bSeverity = SeverityOfStatus(b[DriverCol_Status]);
            if (aSeverity != bSeverity)
            {
                return aSeverity > bSeverity;
            }

            if (int c = StrCmpLogicalW(a[1], b[1]))
            {
                return c < 0;
            }

            return StrCmpLogicalW(a[0], b[0]) < 0;
        });

        table.status.Format(L"%d devices present: %d not working (red), %d disabled (amber). Select a device and use \"Search Update Catalog\" to look up drivers by hardware ID.",
            static_cast<int>(table.rows.size()), problems, disabled);
        return table;
    }

    // ---------------------------------------------------------------------------
    // App updates: run "winget upgrade" and parse its table
    // ---------------------------------------------------------------------------

    DWORD RunAndCapture(CString commandLine, std::string& output, DWORD& exitCode)
    {
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE readPipe = nullptr, writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        {
            return GetLastError();
        }

        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = writePipe;
        si.hStdError = writePipe;

        PROCESS_INFORMATION pi{};
        BOOL started = CreateProcessW(nullptr, commandLine.GetBuffer(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        DWORD error = started ? ERROR_SUCCESS : GetLastError();
        commandLine.ReleaseBuffer();
        CloseHandle(writePipe);

        if (started)
        {
            char buffer[4096];
            DWORD read = 0;
            while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read)
            {
                output.append(buffer, read);
            }

            WaitForSingleObject(pi.hProcess, INFINITE);
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }

        CloseHandle(readPipe);
        return error;
    }

    CString Utf8ToWide(const std::string& text)
    {
        if (text.empty())
        {
            return CString();
        }

        int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        CString wide;
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.GetBuffer(chars), chars);
        wide.ReleaseBuffer(chars);
        return wide;
    }

    // Removes progress-spinner leftovers (carriage returns, backspaces) and ANSI escape sequences.
    CString CleanConsoleLine(CString line)
    {
        line.TrimRight(L"\r");
        int cr = line.ReverseFind(L'\r');
        if (cr >= 0)
        {
            line = line.Mid(cr + 1);
        }

        CString clean;
        for (int i = 0; i < line.GetLength(); ++i)
        {
            wchar_t ch = line[i];
            if (ch == 0x1B && i + 1 < line.GetLength() && line[i + 1] == L'[')
            {
                i += 2;
                while (i < line.GetLength() && !(line[i] >= 0x40 && line[i] <= 0x7E))
                {
                    ++i;
                }

                continue;
            }

            if (ch == L'\b')
            {
                if (!clean.IsEmpty())
                {
                    clean.Delete(clean.GetLength() - 1);
                }

                continue;
            }

            clean += ch;
        }

        return clean;
    }

    ScanTable ScanWingetUpgrades()
    {
        ScanTable table;
        table.columns = ColumnsFor(KindWinget);

        std::string raw;
        DWORD exitCode = 0;
        DWORD error = RunAndCapture(
            L"winget.exe upgrade --include-unknown --accept-source-agreements --disable-interactivity",
            raw, exitCode);
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        {
            table.status = L"winget was not found. Install \"App Installer\" from the Microsoft Store.";
            return table;
        }

        if (error != ERROR_SUCCESS)
        {
            table.status = L"Could not start winget: " + HResultText(HRESULT_FROM_WIN32(error));
            return table;
        }

        std::vector<CString> lines;
        CString text = Utf8ToWide(raw);
        for (int start = 0; start <= text.GetLength();)
        {
            int end = text.Find(L'\n', start);
            if (end < 0)
            {
                end = text.GetLength();
            }

            lines.push_back(CleanConsoleLine(text.Mid(start, end - start)));
            start = end + 1;
        }

        // The table is "header / ------ / rows". Column positions come from the header,
        // so this works regardless of the header language.
        int separator = -1;
        for (int i = 1; i < static_cast<int>(lines.size()); ++i)
        {
            CString trimmed = lines[i];
            trimmed.Trim();
            if (trimmed.GetLength() >= 10 && trimmed.SpanIncluding(L"-") == trimmed)
            {
                separator = i;
                break;
            }
        }

        if (separator < 0)
        {
            CString last;
            for (const auto& line : lines)
            {
                CString trimmed = line;
                if (!trimmed.Trim().IsEmpty())
                {
                    last = trimmed;
                }
            }

            table.status = last.IsEmpty() ? CString(L"winget returned no output.") : L"winget: " + last;
            return table;
        }

        const CString& header = lines[separator - 1];
        std::vector<int> starts;
        for (int i = 0; i < header.GetLength(); ++i)
        {
            if (header[i] != L' ' && (i == 0 || header[i - 1] == L' '))
            {
                starts.push_back(i);
            }
        }

        if (starts.size() < 4)
        {
            table.status = L"Could not understand winget's output: " + header;
            return table;
        }

        for (size_t i = separator + 1; i < lines.size(); ++i)
        {
            const CString& line = lines[i];
            if (line.GetLength() <= starts.back())
            {
                break;  // blank line or the "N upgrades available." summary
            }

            std::vector<CString> fields;
            for (size_t c = 0; c < starts.size(); ++c)
            {
                int begin = starts[c];
                int end = c + 1 < starts.size() ? starts[c + 1] : line.GetLength();
                CString field = line.Mid(begin, end - begin);
                fields.push_back(field.Trim());
            }

            fields.resize(5);   // Name, Id, Version, Available, Source
            table.rows.push_back(fields);
        }

        table.status.Format(L"%d app updates available through winget. Tick apps and click \"Upgrade Checked\".",
            static_cast<int>(table.rows.size()));
        return table;
    }

    // ---------------------------------------------------------------------------
    // Windows Update Agent: pending software and driver updates
    // ---------------------------------------------------------------------------

    void SearchWindowsUpdate(ScanTable& table)
    {
        CComPtr<IUpdateSession> session;
        HRESULT hr = session.CoCreateInstance(CLSID_UpdateSession);
        if (FAILED(hr))
        {
            table.status = L"Could not start the Windows Update Agent: " + HResultText(hr);
            return;
        }

        session->put_ClientApplicationID(CComBSTR(L"Update Inspector"));

        CComPtr<IUpdateSearcher> searcher;
        hr = session->CreateUpdateSearcher(&searcher);
        if (FAILED(hr))
        {
            table.status = L"CreateUpdateSearcher failed: " + HResultText(hr);
            return;
        }

        CComPtr<ISearchResult> result;
        hr = searcher->Search(CComBSTR(L"IsInstalled=0 and IsHidden=0"), &result);
        if (FAILED(hr))
        {
            table.status = L"Windows Update search failed: " + HResultText(hr);
            return;
        }

        CComPtr<IUpdateCollection> updates;
        LONG count = 0;
        if (FAILED(result->get_Updates(&updates)) || FAILED(updates->get_Count(&count)))
        {
            count = 0;
        }

        int drivers = 0;
        for (LONG i = 0; i < count; ++i)
        {
            CComPtr<IUpdate> update;
            if (FAILED(updates->get_Item(i, &update)))
            {
                continue;
            }

            CComBSTR title;
            update->get_Title(&title);
            UpdateType type = utSoftware;
            update->get_Type(&type);

            CString kb;
            CComPtr<IStringCollection> kbIds;
            if (SUCCEEDED(update->get_KBArticleIDs(&kbIds)) && kbIds)
            {
                LONG kbCount = 0;
                kbIds->get_Count(&kbCount);
                for (LONG k = 0; k < kbCount; ++k)
                {
                    CComBSTR id;
                    if (SUCCEEDED(kbIds->get_Item(k, &id)))
                    {
                        kb += (kb.IsEmpty() ? L"KB" : L", KB") + CString(id.m_str);
                    }
                }
            }

            CString size;
            DECIMAL maxSize{};
            double bytes = 0;
            if (SUCCEEDED(update->get_MaxDownloadSize(&maxSize)) && SUCCEEDED(VarR8FromDec(&maxSize, &bytes)) && bytes > 0)
            {
                wchar_t buffer[32];
                StrFormatByteSizeW(static_cast<LONGLONG>(bytes), buffer, _countof(buffer));
                size = buffer;
            }

            CString driverClass, provider, driverDate, hardwareId;
            CComQIPtr<IWindowsDriverUpdate> driver(update.p);
            if (driver)
            {
                CComBSTR value1, value2, value3;
                if (SUCCEEDED(driver->get_DriverClass(&value1)))
                {
                    driverClass = value1.m_str;
                }

                if (SUCCEEDED(driver->get_DriverProvider(&value2)))
                {
                    provider = value2.m_str;
                }

                if (SUCCEEDED(driver->get_DriverHardwareID(&value3)))
                {
                    hardwareId = value3.m_str;
                }

                DATE date = 0;
                SYSTEMTIME st{};
                if (SUCCEEDED(driver->get_DriverVerDate(&date)) && date != 0 && VariantTimeToSystemTime(date, &st))
                {
                    driverDate = FormatDate(st);
                }
            }

            if (type == utDriver)
            {
                ++drivers;
            }

            CString updateId;
            CComPtr<IUpdateIdentity> identity;
            CComBSTR idText;
            if (SUCCEEDED(update->get_Identity(&identity)) && identity && SUCCEEDED(identity->get_UpdateID(&idText)))
            {
                updateId = idText.m_str;
            }

            table.rows.push_back({
                CString(title.m_str),
                type == utDriver ? L"Driver" : L"Software",
                kb,
                size,
                driverClass,
                provider,
                driverDate,
                hardwareId,
                updateId,
                });
        }

        table.status.Format(L"%d updates pending (%d drivers, %d software). Tick updates and click \"Install Checked\".",
            static_cast<int>(count), drivers, static_cast<int>(count) - drivers);
    }

    ScanTable ScanWindowsUpdate()
    {
        ScanTable table;
        table.columns = ColumnsFor(KindWindowsUpdate);

        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        SearchWindowsUpdate(table);
        if (SUCCEEDED(hr))
        {
            CoUninitialize();
        }

        return table;
    }

    // ---------------------------------------------------------------------------
    // Installing: System Restore point + Windows Update Agent download/install
    // ---------------------------------------------------------------------------

    // Brackets a system change with SRSetRestorePoint. srclient.dll is loaded at run time
    // because System Restore is missing on some editions (e.g. Server).
    class RestorePoint
    {
    public:
        ~RestorePoint()
        {
            End(true);
            if (m_library)
            {
                FreeLibrary(m_library);
            }
        }

        // Returns an empty string on success, otherwise why no restore point was created.
        CString Begin(LPCWSTR description)
        {
            m_library = LoadLibraryExW(L"srclient.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (m_library)
            {
                m_setRestorePoint = reinterpret_cast<decltype(&SRSetRestorePointW)>(
                    GetProcAddress(m_library, "SRSetRestorePointW"));
            }

            if (!m_setRestorePoint)
            {
                return L"System Restore is not available on this edition of Windows.";
            }

            RESTOREPOINTINFOW info{};
            info.dwEventType = BEGIN_SYSTEM_CHANGE;
            info.dwRestorePtType = APPLICATION_INSTALL;
            wcsncpy_s(info.szDescription, description, _TRUNCATE);

            STATEMGRSTATUS status{};
            if (!m_setRestorePoint(&info, &status))
            {
                if (status.nStatus == ERROR_SERVICE_DISABLED)
                {
                    return L"System Protection is turned off for the system drive, so no restore point can be created.";
                }

                return L"Could not create a restore point: " + HResultText(HRESULT_FROM_WIN32(status.nStatus));
            }

            m_sequence = status.llSequenceNumber;
            m_active = true;
            return CString();
        }

        // keep = false removes the restore point again (nothing was changed).
        void End(bool keep)
        {
            if (!m_active)
            {
                return;
            }

            RESTOREPOINTINFOW info{};
            info.dwEventType = END_SYSTEM_CHANGE;
            info.dwRestorePtType = keep ? APPLICATION_INSTALL : CANCELLED_OPERATION;
            info.llSequenceNumber = m_sequence;
            STATEMGRSTATUS status{};
            m_setRestorePoint(&info, &status);
            m_active = false;
        }

        bool Active() const
        {
            return m_active;
        }

        INT64 Sequence() const
        {
            return m_sequence;
        }

    private:
        HMODULE m_library = nullptr;
        decltype(&SRSetRestorePointW) m_setRestorePoint = nullptr;
        INT64 m_sequence = 0;
        bool m_active = false;
    };

    CString ResultCodeText(OperationResultCode code)
    {
        switch (code)
        {
        case orcSucceeded:
            return L"installed";

        case orcSucceededWithErrors:
            return L"installed with errors";

        case orcFailed:
            return L"FAILED";

        case orcAborted:
            return L"aborted";

        case orcInProgress:
            return L"in progress";

        default:
            return L"not started";
        }
    }

    CString UpdateTitle(IUpdateCollection* updates, LONG index)
    {
        CComPtr<IUpdate> update;
        CComBSTR title;
        if (SUCCEEDED(updates->get_Item(index, &update)) && SUCCEEDED(update->get_Title(&title)))
        {
            return CString(title.m_str);
        }

        return CString();
    }

    // Returns the summary shown to the user.
    CString InstallUpdates(const std::vector<CString>& updateIds, const ProgressFn& progress, const ConfirmFn& confirm)
    {
        CComPtr<IUpdateSession> session;
        HRESULT hr = session.CoCreateInstance(CLSID_UpdateSession);
        if (FAILED(hr))
        {
            return L"Could not start the Windows Update Agent: " + HResultText(hr);
        }

        session->put_ClientApplicationID(CComBSTR(L"Update Inspector"));

        // 1. Look the updates up again by ID (OR is allowed at the top level of the criteria).
        progress(L"Looking up the selected updates...");
        CString criteria;
        for (const auto& id : updateIds)
        {
            criteria += (criteria.IsEmpty() ? L"" : L" or ") + CString(L"UpdateID='") + id + L"'";
        }

        CComPtr<IUpdateSearcher> searcher;
        CComPtr<ISearchResult> found;
        CComPtr<IUpdateCollection> foundUpdates;
        if (FAILED(hr = session->CreateUpdateSearcher(&searcher))
            || FAILED(hr = searcher->Search(CComBSTR(criteria), &found))
            || FAILED(hr = found->get_Updates(&foundUpdates)))
        {
            return L"Windows Update search failed: " + HResultText(hr);
        }

        CComPtr<IUpdateCollection> toDownload;
        if (FAILED(hr = toDownload.CoCreateInstance(CLSID_UpdateCollection)))
        {
            return L"Could not create an update collection: " + HResultText(hr);
        }

        LONG foundCount = 0;
        foundUpdates->get_Count(&foundCount);
        for (LONG i = 0; i < foundCount; ++i)
        {
            CComPtr<IUpdate> update;
            if (FAILED(foundUpdates->get_Item(i, &update)))
            {
                continue;
            }

            VARIANT_BOOL installed = VARIANT_FALSE, eulaAccepted = VARIANT_TRUE;
            update->get_IsInstalled(&installed);
            if (installed)
            {
                continue;
            }

            update->get_EulaAccepted(&eulaAccepted);
            if (!eulaAccepted)
            {
                update->AcceptEula();   // the user agreed when confirming the install
            }

            LONG index = 0;
            toDownload->Add(update, &index);
        }

        LONG downloadCount = 0;
        toDownload->get_Count(&downloadCount);
        if (downloadCount == 0)
        {
            return L"None of the selected updates is still pending.";
        }

        // 2. Download. This changes nothing on the system yet.
        CString text;
        text.Format(L"Downloading %d update(s)...", downloadCount);
        progress(text);

        CComPtr<IUpdateDownloader> downloader;
        CComPtr<IDownloadResult> downloadResult;
        if (FAILED(hr = session->CreateUpdateDownloader(&downloader))
            || FAILED(hr = downloader->put_Updates(toDownload))
            || FAILED(hr = downloader->Download(&downloadResult)))
        {
            return L"Download failed: " + HResultText(hr);
        }

        CString summary;
        CComPtr<IUpdateCollection> toInstall;
        toInstall.CoCreateInstance(CLSID_UpdateCollection);
        for (LONG i = 0; i < downloadCount; ++i)
        {
            CComPtr<IUpdate> update;
            VARIANT_BOOL downloaded = VARIANT_FALSE;
            if (SUCCEEDED(toDownload->get_Item(i, &update)))
            {
                update->get_IsDownloaded(&downloaded);
            }

            if (downloaded)
            {
                LONG index = 0;
                toInstall->Add(update, &index);
            }
            else
            {
                summary += L"\n- " + UpdateTitle(toDownload, i) + L": download FAILED";
            }
        }

        LONG installCount = 0;
        toInstall->get_Count(&installCount);
        if (installCount == 0)
        {
            return L"No update could be downloaded." + summary;
        }

        // 3. Restore point, so the user can revert the whole install from System Restore.
        progress(L"Creating a System Restore point (this can take a minute)...");
        RestorePoint restorePoint;
        CString restoreError = restorePoint.Begin(L"Update Inspector - before installing Windows updates");
        if (!restoreError.IsEmpty()
            && !confirm(restoreError + L"\n\nInstall the updates anyway, without a restore point?"))
        {
            return L"Installation cancelled because no restore point could be created.\n\n" + restoreError;
        }

        // 4. Install.
        text.Format(L"Installing %d update(s)... do not turn off the computer.", installCount);
        progress(text);

        CComPtr<IUpdateInstaller> installer;
        CComPtr<IInstallationResult> installResult;
        if (FAILED(hr = session->CreateUpdateInstaller(&installer))
            || FAILED(hr = installer->put_Updates(toInstall)))
        {
            restorePoint.End(false);
            return L"Could not start the installer: " + HResultText(hr);
        }

        installer->put_AllowSourcePrompts(VARIANT_FALSE);
        hr = installer->Install(&installResult);
        restorePoint.End(true);
        if (FAILED(hr))
        {
            return L"Installation failed: " + HResultText(hr) + summary;
        }

        int succeeded = 0;
        for (LONG i = 0; i < installCount; ++i)
        {
            CComPtr<IUpdateInstallationResult> result;
            OperationResultCode code = orcNotStarted;
            HRESULT updateHr = S_OK;
            if (SUCCEEDED(installResult->GetUpdateResult(i, &result)))
            {
                result->get_ResultCode(&code);
                result->get_HResult(&updateHr);
            }

            if (code == orcSucceeded || code == orcSucceededWithErrors)
            {
                ++succeeded;
            }

            summary += L"\n- " + UpdateTitle(toInstall, i) + L": " + ResultCodeText(code);
            if (FAILED(updateHr))
            {
                summary += L" (" + HResultText(updateHr) + L")";
            }
        }

        CString header;
        header.Format(L"Installed %d of %d update(s).", succeeded, downloadCount);
        VARIANT_BOOL rebootRequired = VARIANT_FALSE;
        installResult->get_RebootRequired(&rebootRequired);
        if (rebootRequired)
        {
            header += L" Restart Windows to finish.";
        }

        CString footer;
        if (restorePoint.Sequence() != 0)
        {
            footer.Format(L"\n\nRestore point #%I64d was created before installing. To undo everything, run "
                L"rstrui.exe (System Restore) and pick \"Update Inspector - before installing Windows updates\".",
                restorePoint.Sequence());
        }
        else
        {
            footer = L"\n\nNo restore point was created.";
        }

        footer += L"\nA single driver can also be undone in Device Manager > Properties > Driver > Roll Back Driver.";
        return header + summary + footer;
    }

} // namespace

DeviceSeverity SeverityOfStatus(const CString& status)
{
    // Matches the texts DeviceStatusText produces.
    if (status.IsEmpty() || status == L"OK" || status == L"Unknown")
    {
        return SeverityNone;
    }

    if (status.Left(8) == L"Disabled")
    {
        return SeverityDisabled;
    }

    return SeverityProblem;
}

bool IsGuidString(const CString& text)
{
    // 8-4-4-4-12 hex digits, as reported by IUpdateIdentity::UpdateID.
    if (text.GetLength() != 36)
    {
        return false;
    }

    for (int i = 0; i < 36; ++i)
    {
        const wchar_t ch = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (ch != L'-')
            {
                return false;
            }
        }
        else if (!iswxdigit(ch))
        {
            return false;
        }
    }

    return true;
}

ScanTable InstallWindowsUpdates(const std::vector<CString>& updateIds, const ProgressFn& progress,
    const ConfirmFn& confirm)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CString summary = InstallUpdates(updateIds, progress, confirm);

    progress(L"Searching Windows Update again...");
    ScanTable table;
    table.columns = ColumnsFor(KindWindowsUpdate);
    SearchWindowsUpdate(table);
    if (SUCCEEDED(hr))
    {
        CoUninitialize();
    }

    int newline = summary.Find(L'\n');
    table.status = (newline < 0 ? summary : summary.Left(newline)) + L"  " + table.status;
    table.notice = summary;
    return table;
}

std::vector<ListColumn> ColumnsFor(ScanKind kind)
{
    switch (kind)
    {
    case KindSoftware:
        return { { L"Name", 300 }, { L"Version", 120 }, { L"Publisher", 200 }, { L"Installed", 90 },
                 { L"Type", 50 }, { L"Scope", 120 }, { L"Registry Key", 260 } };
    case KindDrivers:
        return { { L"Device", 280 }, { L"Class", 110 }, { L"Driver Version", 120 }, { L"Driver Date", 90 },
                 { L"Provider", 140 }, { L"INF", 90 }, { L"Status", 130 }, { L"Hardware ID", 320 } };
    case KindWinget:
        return { { L"Name", 300 }, { L"Id", 260 }, { L"Installed", 120 }, { L"Available", 120 }, { L"Source", 80 } };
    case KindWindowsUpdate:
        return { { L"Title", 420 }, { L"Type", 70 }, { L"KB", 90 }, { L"Size", 80 },
                 { L"Driver Class", 100 }, { L"Provider", 130 }, { L"Driver Date", 90 }, { L"Hardware ID", 260 },
                 { L"Update ID", 260 } };
    }
    return {};
}

ScanTable RunScan(ScanKind kind)
{
    switch (kind)
    {
    case KindSoftware:
        return ScanInstalledSoftware();

    case KindDrivers:
        return ScanDevicesAndDrivers();

    case KindWinget:
        return ScanWingetUpgrades();

    case KindWindowsUpdate:
        return ScanWindowsUpdate();
    }

    return ScanTable();
}
