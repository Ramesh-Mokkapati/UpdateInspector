#pragma once

#include "Scanners.h"

#include <set>

class CMainDlg : public CDialogEx
{
public:
    explicit CMainDlg(CWnd* parent = nullptr);
    ~CMainDlg() override;

    // Set by InitInstance when started elevated with /install-wu <UpdateID>...
    void InstallOnStart(std::vector<CString> updateIds)
    {
        m_installOnStart = std::move(updateIds);
    }

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    BOOL OnNotify(WPARAM wParam, LPARAM lParam, LRESULT* result) override;
    void OnOK() override {}     // Enter must not close the window
    void OnCancel() override;

    afx_msg void OnSize(UINT type, int cx, int cy);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* info);
    afx_msg BOOL OnSetCursor(CWnd* wnd, UINT hitTest, UINT message);
    afx_msg void OnTabChanged(NMHDR* nmhdr, LRESULT* result);
    afx_msg void OnColumnClick(NMHDR* nmhdr, LRESULT* result);
    afx_msg void OnItemChanged(NMHDR* nmhdr, LRESULT* result);
    afx_msg void OnListCustomDraw(NMHDR* nmhdr, LRESULT* result);
    afx_msg void OnScan();
    afx_msg void OnAction();
    afx_msg void OnInstall();
    afx_msg void OnExport();
    afx_msg LRESULT OnScanDone(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnJobProgress(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnElevatedDone(WPARAM wParam, LPARAM lParam);
    DECLARE_MESSAGE_MAP()

private:
    using Job = std::function<ScanTable(const ProgressFn&)>;

    int Px(int value) const;
    void Layout(int cx, int cy);
    void ShowTab(ScanKind kind);
    void FillList();
    void UpdateUi();
    void UpdateHeaderCheck();
    void RefreshCursor();
    void SetAllChecked(bool checked);
    void StartScan(ScanKind kind);
    void StartJob(ScanKind kind, Job job);
    std::vector<int> SelectedRows() const;
    std::vector<int> RowsToAct(ScanKind kind) const;   // checked rows, else the selection

    void SearchUpdateCatalog();
    void UpgradeWithWinget();
    void StartInstall(const std::vector<CString>& updateIds);
    void LaunchElevatedInstall(const std::vector<CString>& updateIds);

    HICON m_hIcon = nullptr;
    HICON m_hSmallIcon = nullptr;
    CTabCtrl m_tabs;
    CListCtrl m_list;
    CStatic m_status;
    CButton m_scan;
    CButton m_action;
    CButton m_install;
    CButton m_export;
    CButton m_close;

    ScanKind m_tab = KindSoftware;
    ScanTable m_tables[KindCount];
    bool m_busy[KindCount] = {};
    CString m_progress[KindCount];
    std::set<CString> m_checked[KindCount];     // keys (winget Id / UpdateID) of ticked rows
    int m_sortColumn[KindCount] = { -1, -1, -1, -1 };
    bool m_sortAscending[KindCount] = { true, true, true, true };
    bool m_filling = false;                     // ignore check-state notifications while refilling
    bool m_elevated = false;
    bool m_installing = false;
    std::vector<CString> m_installOnStart;
};
