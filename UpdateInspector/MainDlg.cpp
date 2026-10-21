#include "framework.h"
#include "resource.h"
#include "MainDlg.h"

#include <shlwapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <memory>
#include <thread>

namespace
{
    const UINT WM_SCAN_DONE = WM_APP + 1;       // wParam = ScanKind, lParam = ScanTable* (receiver deletes)
    const UINT WM_JOB_PROGRESS = WM_APP + 2;    // wParam = ScanKind, lParam = CString* (receiver deletes)
    const UINT WM_ELEVATED_DONE = WM_APP + 3;   // the elevated installer window has closed

    const wchar_t* const kTabTitles[KindCount] =
    {
        L"Installed Software",
        L"Devices and Drivers",
        L"App Updates (winget)",
        L"Windows Update",
    };

    const wchar_t* const kIdleText[KindCount] =
    {
        L"Click Scan to list installed programs.",
        L"Click Scan to list devices and their drivers.",
        L"Click Scan to ask winget which installed apps have newer versions.",
        L"Click Scan to search Windows Update for pending software and driver updates (can take a few minutes).",
    };

    const wchar_t* const kBusyText[KindCount] =
    {
        L"Reading the Uninstall registry keys...",
        L"Enumerating devices with SetupAPI...",
        L"Running \"winget upgrade\"...",
        L"Searching Windows Update... this can take a few minutes.",
    };

    const wchar_t* const kActionText[KindCount] =
    {
        L"Open Apps Settings",
        L"Search Update &Catalog",
        L"&Upgrade Checked",
        L"Open Windows &Update",
    };

    const wchar_t* const kExportName[KindCount] =
    {
        L"installed-software.csv",
        L"devices-and-drivers.csv",
        L"winget-upgrades.csv",
        L"windows-update.csv",
    };

    // The column that identifies a row on tabs with checkboxes; -1 = no checkboxes.
    int KeyColumn(ScanKind kind)
    {
        switch (kind)
        {
        case KindWinget:
            return WingetCol_Id;

        case KindWindowsUpdate:
            return WuCol_UpdateId;

        default:
            return -1;
        }
    }

    bool IsProcessElevated()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        {
            return false;
        }

        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
        CloseHandle(token);
        return ok && elevation.TokenIsElevated;
    }

    bool IsSafePackageId(const CString& id)
    {
        if (id.IsEmpty())
        {
            return false;
        }

        for (int i = 0; i < id.GetLength(); ++i)
        {
            wchar_t ch = id[i];
            bool ok = (ch < 128 && iswalnum(ch)) || ch == L'.' || ch == L'-' || ch == L'_' || ch == L'+';
            if (!ok)
            {
                return false;
            }
        }

        return true;
    }

    CString UrlEncode(const CString& text)
    {
        CW2A utf8(text, CP_UTF8);
        CString encoded;
        for (const char* p = utf8; *p; ++p)
        {
            unsigned char ch = static_cast<unsigned char>(*p);
            if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            {
                encoded += static_cast<wchar_t>(ch);
            }
            else
            {
                encoded.AppendFormat(L"%%%02X", ch);
            }
        }

        return encoded;
    }

    // True for devices that software creates rather than a bus finding hardware:
    // IDs without a bus prefix ("VPNVA", "*NXVVC") and the software enumerators.
    bool IsSoftwareDeviceId(const CString& hardwareId)
    {
        const int slash = hardwareId.Find(L'\\');
        if (slash <= 0 || hardwareId[0] == L'{' || hardwareId[0] == L'*')
        {
            return true;
        }

        CString bus = hardwareId.Left(slash);
        bus.MakeUpper();
        for (const wchar_t* software : { L"ROOT", L"SWD", L"SW", L"HTREE", L"MMDEVAPI", L"COMPUTER", L"BUTTONCONVERTER" })
        {
            if (bus == software)
            {
                return true;
            }
        }

        return false;
    }

    CString CsvField(const CString& value)
    {
        CString escaped = value;
        escaped.Replace(L"\"", L"\"\"");
        return L"\"" + escaped + L"\"";
    }

} // namespace

BEGIN_MESSAGE_MAP(CMainDlg, CDialogEx)
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
    ON_WM_SETCURSOR()
    ON_NOTIFY(TCN_SELCHANGE, IDC_TABS, &CMainDlg::OnTabChanged)
    ON_NOTIFY(LVN_COLUMNCLICK, IDC_LIST, &CMainDlg::OnColumnClick)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_LIST, &CMainDlg::OnItemChanged)
    ON_NOTIFY(NM_CUSTOMDRAW, IDC_LIST, &CMainDlg::OnListCustomDraw)
    ON_BN_CLICKED(IDC_SCAN, &CMainDlg::OnScan)
    ON_BN_CLICKED(IDC_ACTION, &CMainDlg::OnAction)
    ON_BN_CLICKED(IDC_INSTALL, &CMainDlg::OnInstall)
    ON_BN_CLICKED(IDC_EXPORT, &CMainDlg::OnExport)
    ON_MESSAGE(WM_SCAN_DONE, &CMainDlg::OnScanDone)
    ON_MESSAGE(WM_JOB_PROGRESS, &CMainDlg::OnJobProgress)
    ON_MESSAGE(WM_ELEVATED_DONE, &CMainDlg::OnElevatedDone)
END_MESSAGE_MAP()

CMainDlg::CMainDlg(CWnd* parent)
    : CDialogEx(IDD_MAIN, parent)
{
}

CMainDlg::~CMainDlg()
{
    if (m_hIcon)
    {
        DestroyIcon(m_hIcon);
    }

    if (m_hSmallIcon)
    {
        DestroyIcon(m_hSmallIcon);
    }
}

void CMainDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_TABS, m_tabs);
    DDX_Control(pDX, IDC_LIST, m_list);
    DDX_Control(pDX, IDC_STATUS, m_status);
    DDX_Control(pDX, IDC_SCAN, m_scan);
    DDX_Control(pDX, IDC_ACTION, m_action);
    DDX_Control(pDX, IDC_INSTALL, m_install);
    DDX_Control(pDX, IDC_EXPORT, m_export);
    DDX_Control(pDX, IDCANCEL, m_close);
}

BOOL CMainDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    // Load each size exactly, so the title bar and taskbar get a crisp image at this DPI.
    const UINT dpi = GetDpiForWindow(m_hWnd);
    LoadIconWithScaleDown(AfxGetResourceHandle(), MAKEINTRESOURCE(IDR_MAINFRAME),
        GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi), &m_hIcon);
    LoadIconWithScaleDown(AfxGetResourceHandle(), MAKEINTRESOURCE(IDR_MAINFRAME),
        GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), &m_hSmallIcon);
    SetIcon(m_hIcon, TRUE);
    SetIcon(m_hSmallIcon, FALSE);

    m_elevated = IsProcessElevated();
    if (m_elevated)
    {
        SetWindowText(L"Update Inspector (Administrator)");
    }
    else
    {
        m_install.SetShield(TRUE);  // installing will ask for elevation
    }

    for (int kind = 0; kind < KindCount; ++kind)
    {
        m_tabs.InsertItem(kind, kTabTitles[kind]);
        m_tables[kind].columns = ColumnsFor(static_cast<ScanKind>(kind));
        m_tables[kind].status = kIdleText[kind];
    }

    m_list.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_HEADERDRAGDROP);
    SetWindowTheme(m_list.GetSafeHwnd(), L"Explorer", nullptr);

    // The local scans are quick; winget and Windows Update run only on request.
    StartScan(KindSoftware);
    StartScan(KindDrivers);

    if (!m_installOnStart.empty())
    {
        ShowTab(KindWindowsUpdate);
        StartInstall(m_installOnStart);
    }
    else
    {
        ShowTab(KindSoftware);
    }

    return TRUE;
}

void CMainDlg::OnCancel()
{
    if (m_installing
        && AfxMessageBox(L"Updates are still being installed. Closing now can leave them half-installed.\n\nClose anyway?",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    {
        return;
    }

    CDialogEx::OnCancel();
}

int CMainDlg::Px(int value) const
{
    return MulDiv(value, GetDpiForWindow(m_hWnd), 96);
}

void CMainDlg::OnSize(UINT type, int cx, int cy)
{
    CDialogEx::OnSize(type, cx, cy);
    if (type != SIZE_MINIMIZED)
    {
        Layout(cx, cy);
    }
}

void CMainDlg::OnGetMinMaxInfo(MINMAXINFO* info)
{
    if (m_hWnd)
    {
        info->ptMinTrackSize.x = Px(760);
        info->ptMinTrackSize.y = Px(400);
    }

    CDialogEx::OnGetMinMaxInfo(info);
}

void CMainDlg::Layout(int cx, int cy)
{
    if (!m_list.GetSafeHwnd())
    {
        return;
    }

    const int margin = Px(10), gap = Px(6), buttonHeight = Px(28);

    int tabHeight = Px(26);
    CRect tabItem;
    if (m_tabs.GetItemCount() > 0 && m_tabs.GetItemRect(0, &tabItem))
    {
        tabHeight = tabItem.bottom + Px(2);
    }

    m_tabs.MoveWindow(margin, margin, cx - 2 * margin, tabHeight);

    const int listTop = margin + tabHeight;
    const int buttonTop = cy - margin - buttonHeight;
    m_list.MoveWindow(margin, listTop, cx - 2 * margin, (std::max)(0, buttonTop - gap - listTop));

    int x = cx - margin;
    auto place = [&](CWnd& button, int width)
    {
        x -= width;
        button.MoveWindow(x, buttonTop, width, buttonHeight);
        x -= gap;
    };

    place(m_close, Px(80));
    place(m_export, Px(100));
    place(m_action, Px(150));
    const bool showInstall = m_tab == KindWindowsUpdate;
    m_install.ShowWindow(showInstall ? SW_SHOW : SW_HIDE);

    if (showInstall)
    {
        place(m_install, Px(130));
    }

    place(m_scan, Px(80));

    const int statusHeight = Px(18);
    m_status.MoveWindow(margin, buttonTop + (buttonHeight - statusHeight) / 2,
        (std::max)(0, x - margin), statusHeight);
}

void CMainDlg::OnTabChanged(NMHDR*, LRESULT* result)
{
    ShowTab(static_cast<ScanKind>(m_tabs.GetCurSel()));
    *result = 0;
}

void CMainDlg::ShowTab(ScanKind kind)
{
    m_tab = kind;
    if (m_tabs.GetCurSel() != kind)
    {
        m_tabs.SetCurSel(kind);
    }

    CRect client;
    GetClientRect(&client);
    Layout(client.Width(), client.Height());
    FillList();
}

void CMainDlg::FillList()
{
    const ScanTable& table = m_tables[m_tab];
    const int keyColumn = KeyColumn(m_tab);
    const bool checkboxes = keyColumn >= 0;
    CHeaderCtrl* header = m_list.GetHeaderCtrl();

    m_filling = true;
    m_list.SetRedraw(FALSE);
    m_list.DeleteAllItems();
    while (m_list.DeleteColumn(0))
    {
    }

    m_list.SetExtendedStyle((m_list.GetExtendedStyle() & ~LVS_EX_CHECKBOXES) | (checkboxes ? LVS_EX_CHECKBOXES : 0));
    header->ModifyStyle(checkboxes ? 0 : HDS_CHECKBOXES, checkboxes ? HDS_CHECKBOXES : 0);

    const int columnCount = static_cast<int>(table.columns.size());
    for (int c = 0; c < columnCount; ++c)
    {
        m_list.InsertColumn(c, table.columns[c].title, LVCFMT_LEFT, Px(table.columns[c].width));
    }

    for (int r = 0; r < static_cast<int>(table.rows.size()); ++r)
    {
        const auto& row = table.rows[r];
        int item = m_list.InsertItem(r, row.empty() ? CString() : row[0]);
        for (int c = 1; c < columnCount && c < static_cast<int>(row.size()); ++c)
        {
            m_list.SetItemText(item, c, row[c]);
        }

        if (checkboxes && keyColumn < static_cast<int>(row.size()) && m_checked[m_tab].count(row[keyColumn]))
        {
            m_list.SetCheck(item, TRUE);
        }
    }

    const int sortColumn = m_sortColumn[m_tab];
    if (sortColumn >= 0 && sortColumn < columnCount)
    {
        HDITEM headerItem{};
        headerItem.mask = HDI_FORMAT;
        header->GetItem(sortColumn, &headerItem);
        headerItem.fmt |= m_sortAscending[m_tab] ? HDF_SORTUP : HDF_SORTDOWN;
        header->SetItem(sortColumn, &headerItem);
    }

    m_filling = false;
    UpdateHeaderCheck();
    m_list.SetRedraw(TRUE);
    m_list.Invalidate();
    UpdateUi();
}

// Keeps the "check all" box in the first column header in step with the rows.
void CMainDlg::UpdateHeaderCheck()
{
    if (KeyColumn(m_tab) < 0 || m_list.GetHeaderCtrl()->GetItemCount() == 0)
    {
        return;
    }

    const int count = m_list.GetItemCount();
    bool all = count > 0;
    for (int i = 0; i < count && all; ++i)
    {
        all = m_list.GetCheck(i) != FALSE;
    }

    CHeaderCtrl* header = m_list.GetHeaderCtrl();
    HDITEM headerItem{};
    headerItem.mask = HDI_FORMAT;
    header->GetItem(0, &headerItem);
    headerItem.fmt = (headerItem.fmt & ~HDF_CHECKED) | HDF_CHECKBOX | (all ? HDF_CHECKED : 0);
    header->SetItem(0, &headerItem);
}

void CMainDlg::SetAllChecked(bool checked)
{
    const int keyColumn = KeyColumn(m_tab);
    if (keyColumn < 0)
    {
        return;
    }

    m_filling = true;
    const auto& rows = m_tables[m_tab].rows;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
    {
        m_list.SetCheck(i, checked);
        const CString& key = rows[i][keyColumn];
        if (key.IsEmpty())
        {
            continue;
        }

        if (checked)
        {
            m_checked[m_tab].insert(key);
        }
        else
        {
            m_checked[m_tab].erase(key);
        }
    }

    m_filling = false;
    UpdateHeaderCheck();
    UpdateUi();
}

BOOL CMainDlg::OnNotify(WPARAM wParam, LPARAM lParam, LRESULT* result)
{
    // The list view forwards its header's notifications to us.
    const auto* nmhdr = reinterpret_cast<const NMHDR*>(lParam);
    CHeaderCtrl* header = m_list.GetSafeHwnd() ? m_list.GetHeaderCtrl() : nullptr;
    if (header && nmhdr->hwndFrom == header->m_hWnd && nmhdr->code == HDN_ITEMSTATEICONCLICK)
    {
        HDITEM headerItem{};
        headerItem.mask = HDI_FORMAT;
        header->GetItem(0, &headerItem);
        SetAllChecked(!(headerItem.fmt & HDF_CHECKED));
        *result = 0;
        return TRUE;
    }

    return CDialogEx::OnNotify(wParam, lParam, result);
}

void CMainDlg::UpdateUi()
{
    const bool busy = m_busy[m_tab];
    CString status = m_tables[m_tab].status;
    if (busy)
    {
        status = m_progress[m_tab].IsEmpty() ? CString(kBusyText[m_tab]) : m_progress[m_tab];
    }

    m_status.SetWindowText(status);
    m_scan.EnableWindow(!busy);
    m_export.EnableWindow(!m_tables[m_tab].rows.empty());

    m_action.SetWindowText(kActionText[m_tab]);
    switch (m_tab)
    {
    case KindDrivers:
        m_action.EnableWindow(!busy && m_list.GetSelectedCount() > 0);
        break;
    case KindWinget:
        m_action.EnableWindow(!busy && !RowsToAct(KindWinget).empty());
        break;
    default:
        m_action.EnableWindow(TRUE);
        break;
    }

    m_install.SetWindowText(m_tab == KindWindowsUpdate && !m_checked[m_tab].empty()
        ? L"&Install Checked" : L"&Install Selected");
    m_install.EnableWindow(m_tab == KindWindowsUpdate && !busy && !RowsToAct(KindWindowsUpdate).empty());

    RefreshCursor();
}

// Busy cursors while scans run on worker threads: the hourglass over the list of the tab
// being scanned, and the "working in background" arrow+hourglass over the rest of the
// window, since the buttons and other tabs stay usable.
BOOL CMainDlg::OnSetCursor(CWnd* wnd, UINT hitTest, UINT message)
{
    if (hitTest == HTCLIENT)
    {
        if (m_busy[m_tab] && wnd && wnd->m_hWnd == m_list.m_hWnd)
        {
            ::SetCursor(::LoadCursor(nullptr, IDC_WAIT));
            return TRUE;
        }

        if (std::any_of(std::begin(m_busy), std::end(m_busy), [](bool busy) { return busy; }))
        {
            ::SetCursor(::LoadCursor(nullptr, IDC_APPSTARTING));
            return TRUE;
        }
    }

    return CDialogEx::OnSetCursor(wnd, hitTest, message);
}

// Windows only asks for a new cursor when the mouse moves; ask now so it changes
// as soon as a scan starts or finishes.
void CMainDlg::RefreshCursor()
{
    CPoint point;
    if (!GetCursorPos(&point))
    {
        return;
    }

    HWND under = ::WindowFromPoint(point);
    if (under && (under == m_hWnd || ::IsChild(m_hWnd, under)))
    {
        ::SendMessage(under, WM_SETCURSOR, reinterpret_cast<WPARAM>(under), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    }
}

void CMainDlg::OnItemChanged(NMHDR* nmhdr, LRESULT* result)
{
    const auto* change = reinterpret_cast<NMLISTVIEW*>(nmhdr);
    *result = 0;
    if (!(change->uChanged & LVIF_STATE))
    {
        return;
    }

    const int keyColumn = KeyColumn(m_tab);
    const bool checkChanged = ((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK) != 0;
    const auto& rows = m_tables[m_tab].rows;
    if (!m_filling && checkChanged && keyColumn >= 0 && change->iItem >= 0
        && change->iItem < static_cast<int>(rows.size()))
    {
        const CString& key = rows[change->iItem][keyColumn];
        if (!key.IsEmpty())
        {
            if ((change->uNewState & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2))
            {
                m_checked[m_tab].insert(key);
            }
            else
            {
                m_checked[m_tab].erase(key);
            }
        }

        UpdateHeaderCheck();
    }

    if (!m_filling)
    {
        UpdateUi();
    }
}

// Tints device rows by severity: red = not working (no driver, cannot start, ...), amber = disabled.
void CMainDlg::OnListCustomDraw(NMHDR* nmhdr, LRESULT* result)
{
    auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(nmhdr);
    *result = CDRF_DODEFAULT;

    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
    {
        if (m_tab == KindDrivers)
        {
            *result = CDRF_NOTIFYITEMDRAW;
        }

        return;
    }

    if (draw->nmcd.dwDrawStage != CDDS_ITEMPREPAINT)
    {
        return;
    }

    const auto& rows = m_tables[KindDrivers].rows;
    const size_t row = draw->nmcd.dwItemSpec;
    if (row >= rows.size())
    {
        return;
    }

    switch (SeverityOfStatus(rows[row][DriverCol_Status]))
    {
    case SeverityProblem:
        draw->clrTextBk = RGB(253, 228, 230);
        draw->clrText = RGB(164, 28, 36);
        break;
    case SeverityDisabled:
        draw->clrTextBk = RGB(255, 243, 205);
        draw->clrText = RGB(122, 86, 0);
        break;
    default:
        break;
    }
}

void CMainDlg::OnColumnClick(NMHDR* nmhdr, LRESULT* result)
{
    const int column = reinterpret_cast<NMLISTVIEW*>(nmhdr)->iSubItem;
    if (m_sortColumn[m_tab] == column)
    {
        m_sortAscending[m_tab] = !m_sortAscending[m_tab];
    }
    else
    {
        m_sortColumn[m_tab] = column;
        m_sortAscending[m_tab] = true;
    }

    const bool ascending = m_sortAscending[m_tab];
    auto& rows = m_tables[m_tab].rows;
    std::stable_sort(rows.begin(), rows.end(), [column, ascending](const auto& a, const auto& b) {
        const wchar_t* left = column < static_cast<int>(a.size()) ? a[column].GetString() : L"";
        const wchar_t* right = column < static_cast<int>(b.size()) ? b[column].GetString() : L"";
        const int order = StrCmpLogicalW(left, right);   // "10.2" sorts after "9.1"
        return ascending ? order < 0 : order > 0;
    });

    FillList();
    *result = 0;
}

void CMainDlg::OnScan()
{
    StartScan(m_tab);
}

void CMainDlg::StartScan(ScanKind kind)
{
    StartJob(kind, [kind](const ProgressFn&) { return RunScan(kind); });
}

void CMainDlg::StartJob(ScanKind kind, Job job)
{
    if (m_busy[kind])
    {
        return;
    }

    m_busy[kind] = true;
    m_progress[kind].Empty();
    UpdateUi();

    HWND hwnd = m_hWnd;
    std::thread([hwnd, kind, job = std::move(job)]
    {
        ProgressFn progress = [hwnd, kind](const CString& text)
        {
            auto message = std::make_unique<CString>(text);
            if (::PostMessage(hwnd, WM_JOB_PROGRESS, kind, reinterpret_cast<LPARAM>(message.get())))
            {
                message.release();
            }
        };

        auto table = std::make_unique<ScanTable>();
        try
        {
            *table = job(progress);
        }
        catch (CException* e)
        {
            wchar_t message[256] = L"";
            e->GetErrorMessage(message, _countof(message));
            e->Delete();
            table->columns = ColumnsFor(kind);
            table->status = CString(L"Failed: ") + message;
        }
        catch (...)
        {
            table->columns = ColumnsFor(kind);
            table->status = L"Failed with an unexpected error.";
        }

        if (::PostMessage(hwnd, WM_SCAN_DONE, kind, reinterpret_cast<LPARAM>(table.get())))
        {
            table.release();
        }
    }).detach();
}

LRESULT CMainDlg::OnJobProgress(WPARAM wParam, LPARAM lParam)
{
    std::unique_ptr<CString> text(reinterpret_cast<CString*>(lParam));
    m_progress[wParam] = *text;
    if (static_cast<ScanKind>(wParam) == m_tab)
    {
        UpdateUi();
    }

    return 0;
}

LRESULT CMainDlg::OnScanDone(WPARAM wParam, LPARAM lParam)
{
    std::unique_ptr<ScanTable> table(reinterpret_cast<ScanTable*>(lParam));
    const auto kind = static_cast<ScanKind>(wParam);

    m_busy[kind] = false;
    m_progress[kind].Empty();
    m_tables[kind] = std::move(*table);
    m_sortColumn[kind] = -1;     // scanners return their own default order
    if (kind == KindWindowsUpdate)
    {
        m_installing = false;
    }

    if (kind == m_tab)
    {
        FillList();
    }

    if (!m_tables[kind].notice.IsEmpty())
    {
        ShowTab(kind);
        AfxMessageBox(m_tables[kind].notice, MB_ICONINFORMATION);
    }

    return 0;
}

std::vector<int> CMainDlg::SelectedRows() const
{
    std::vector<int> rows;
    POSITION pos = m_list.GetFirstSelectedItemPosition();
    while (pos)
    {
        rows.push_back(m_list.GetNextSelectedItem(pos));
    }

    return rows;
}

std::vector<int> CMainDlg::RowsToAct(ScanKind kind) const
{
    std::vector<int> rows;
    const int keyColumn = KeyColumn(kind);
    const auto& tableRows = m_tables[kind].rows;
    for (int i = 0; keyColumn >= 0 && i < static_cast<int>(tableRows.size()); ++i)
    {
        if (m_checked[kind].count(tableRows[i][keyColumn]))
        {
            rows.push_back(i);
        }
    }

    if (rows.empty() && kind == m_tab)
    {
        rows = SelectedRows();
    }

    return rows;
}

void CMainDlg::OnAction()
{
    switch (m_tab)
    {
    case KindSoftware:
        ShellExecuteW(m_hWnd, L"open", L"ms-settings:appsfeatures", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case KindDrivers:
        SearchUpdateCatalog();
        break;
    case KindWinget:
        UpgradeWithWinget();
        break;
    case KindWindowsUpdate:
        ShellExecuteW(m_hWnd, L"open", L"ms-settings:windowsupdate", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
}

void CMainDlg::SearchUpdateCatalog()
{
    const auto selected = SelectedRows();
    if (selected.empty())
    {
        return;
    }

    const auto& row = m_tables[KindDrivers].rows[selected.front()];
    CString hardwareId = row[DriverCol_HardwareId];
    if (hardwareId.IsEmpty())
    {
        AfxMessageBox(L"This device does not report a hardware ID.", MB_ICONINFORMATION);
        return;
    }

    if (IsSoftwareDeviceId(hardwareId))
    {
        CString provider = row[DriverCol_Provider];
        CString message;
        message.Format(L"\"%s\" is a virtual device created by software (hardware ID \"%s\"), not a physical device.\n\n"
            L"The Microsoft Update Catalog only lists drivers for hardware, so it has nothing for this device. "
            L"Its driver is updated together with the software that installed it%s.",
            row[0].GetString(), hardwareId.GetString(),
            provider.IsEmpty() ? L"" : (L" (from " + provider + L")").GetString());
        AfxMessageBox(message, MB_ICONINFORMATION);
        return;
    }

    // The catalog search is text based: the vendor/device part matches far more
    // packages than the full ID with SUBSYS/REV, so cut those off.
    CString upper = hardwareId;
    upper.MakeUpper();
    for (const wchar_t* suffix : { L"&SUBSYS_", L"&REV_" })
    {
        int cut = upper.Find(suffix);
        if (cut > 0)
        {
            hardwareId = hardwareId.Left(cut);
            upper = upper.Left(cut);
        }
    }

    CString url = L"https://www.catalog.update.microsoft.com/Search.aspx?q=" + UrlEncode(hardwareId);
    ShellExecuteW(m_hWnd, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

void CMainDlg::UpgradeWithWinget()
{
    CString commands, names, skipped;
    for (int row : RowsToAct(KindWinget))
    {
        const auto& fields = m_tables[KindWinget].rows[row];
        if (IsSafePackageId(fields[WingetCol_Id]))
        {
            commands += L"winget upgrade --id " + fields[WingetCol_Id]
                + L" -e --accept-package-agreements --accept-source-agreements & ";
            names += L"\n  " + fields[WingetCol_Name];
        }
        else
        {
            skipped += L"\n  " + fields[WingetCol_Name] + L" (" + fields[WingetCol_Id] + L")";
        }
    }

    if (commands.IsEmpty())
    {
        AfxMessageBox(L"None of the chosen packages has a usable winget Id (it may have been truncated)." + skipped,
            MB_ICONWARNING);
        return;
    }

    CString prompt = L"Upgrade these apps with winget? Installers may ask for administrator approval." + names;
    if (!skipped.IsEmpty())
    {
        prompt += L"\n\nSkipped (Id truncated or unusual):" + skipped;
    }

    prompt += L"\n\nNote: winget does not create a System Restore point.";
    if (AfxMessageBox(prompt, MB_YESNO | MB_ICONQUESTION) != IDYES)
    {
        return;
    }

    // A visible console so the user can follow progress and see any errors.
    CString parameters = L"/k " + commands + L"echo. & echo Finished. Close this window and click Scan to refresh.";
    ShellExecuteW(m_hWnd, L"open", L"cmd.exe", parameters, nullptr, SW_SHOWNORMAL);
}

void CMainDlg::OnInstall()
{
    std::vector<CString> updateIds;
    CString titles;
    for (int row : RowsToAct(KindWindowsUpdate))
    {
        const auto& fields = m_tables[KindWindowsUpdate].rows[row];
        if (IsGuidString(fields[WuCol_UpdateId]))
        {
            updateIds.push_back(fields[WuCol_UpdateId]);
            titles += L"\n  " + fields[WuCol_Title];
        }
    }

    if (updateIds.empty())
    {
        return;
    }

    CString prompt = L"Download and install these updates?" + titles
        + L"\n\nA System Restore point is created first, so the whole install can be undone."
        L"\nAny license terms (EULAs) of these updates will be accepted."
        L"\nSome updates need a restart to finish.";

    if (!m_elevated)
    {
        prompt += L"\n\nWindows will ask for administrator permission.";
    }

    if (AfxMessageBox(prompt, MB_YESNO | MB_ICONQUESTION) != IDYES)
    {
        return;
    }

    if (m_elevated)
    {
        StartInstall(updateIds);
    }
    else
    {
        LaunchElevatedInstall(updateIds);
    }
}

void CMainDlg::StartInstall(const std::vector<CString>& updateIds)
{
    m_installing = true;
    HWND hwnd = m_hWnd;
    StartJob(KindWindowsUpdate, [updateIds, hwnd](const ProgressFn& progress)
    {
        ConfirmFn confirm = [hwnd](const CString& question)
        {
            return ::MessageBoxW(hwnd, question, L"Update Inspector", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
        };

        return InstallWindowsUpdates(updateIds, progress, confirm);
    });
}

// Installing needs administrator rights: run a second, elevated copy of this program
// that installs the given updates, and refresh this window when it closes.
void CMainDlg::LaunchElevatedInstall(const std::vector<CString>& updateIds)
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, _countof(exePath));

    CString parameters = L"/install-wu";
    for (const auto& id : updateIds)
    {
        parameters += L" " + id;
    }

    SHELLEXECUTEINFOW info{ sizeof(info) };
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = m_hWnd;
    info.lpVerb = L"runas";
    info.lpFile = exePath;
    info.lpParameters = parameters;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info))
    {
        if (GetLastError() == ERROR_CANCELLED)
        {
            m_tables[KindWindowsUpdate].status = L"Installation cancelled: administrator permission was not given.";
        }
        else
        {
            m_tables[KindWindowsUpdate].status.Format(L"Could not start the elevated installer (error %u).", GetLastError());
        }

        UpdateUi();
        return;
    }

    m_tables[KindWindowsUpdate].status =
        L"Installing in the \"Update Inspector (Administrator)\" window. This list refreshes when that window closes.";
    UpdateUi();

    if (info.hProcess)
    {
        HWND hwnd = m_hWnd;
        HANDLE process = info.hProcess;
        std::thread([hwnd, process]
        {
            WaitForSingleObject(process, INFINITE);
            CloseHandle(process);
            ::PostMessage(hwnd, WM_ELEVATED_DONE, 0, 0);
        }).detach();
    }
}

LRESULT CMainDlg::OnElevatedDone(WPARAM, LPARAM)
{
    m_checked[KindWindowsUpdate].clear();
    StartScan(KindWindowsUpdate);
    return 0;
}

void CMainDlg::OnExport()
{
    const ScanTable& table = m_tables[m_tab];
    CFileDialog dialog(FALSE, L"csv", kExportName[m_tab], OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
        L"CSV files (*.csv)|*.csv|All files (*.*)|*.*||", this);
    if (dialog.DoModal() != IDOK)
    {
        return;
    }

    CString text;
    for (size_t c = 0; c < table.columns.size(); ++c)
    {
        text += (c ? L"," : L"") + CsvField(table.columns[c].title);
    }

    text += L"\r\n";
    for (const auto& row : table.rows)
    {
        for (size_t c = 0; c < row.size(); ++c)
        {
            text += (c ? L"," : L"") + CsvField(row[c]);
        }

        text += L"\r\n";
    }

    CW2A utf8(text, CP_UTF8);
    CFile file;
    if (!file.Open(dialog.GetPathName(), CFile::modeCreate | CFile::modeWrite))
    {
        AfxMessageBox(L"Could not create " + dialog.GetPathName(), MB_ICONERROR);
        return;
    }
    try
    {
        static const BYTE bom[] = { 0xEF, 0xBB, 0xBF };   // lets Excel detect UTF-8
        file.Write(bom, sizeof(bom));
        file.Write(static_cast<const char*>(utf8), static_cast<UINT>(strlen(utf8)));
    }
    catch (CFileException* e)
    {
        e->ReportError();
        e->Delete();
    }
}
