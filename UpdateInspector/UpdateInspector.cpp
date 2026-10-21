#include "framework.h"
#include "MainDlg.h"

class CUpdateInspectorApp : public CWinApp
{
public:
    BOOL InitInstance() override;
};

CUpdateInspectorApp theApp;

namespace
{
    // "UpdateInspector.exe /install-wu <UpdateID> [<UpdateID>...]" is how the normal
    // window starts its elevated copy to install updates.
    std::vector<CString> UpdateIdsFromCommandLine()
    {
        std::vector<CString> ids;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return ids;
        }

        if (argc >= 3 && _wcsicmp(argv[1], L"/install-wu") == 0)
        {
            for (int i = 2; i < argc; ++i)
            {
                if (IsGuidString(argv[i]))
                {
                    ids.emplace_back(argv[i]);
                }
            }
        }

        LocalFree(argv);
        return ids;
    }

} // namespace

BOOL CUpdateInspectorApp::InitInstance()
{
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    CWinApp::InitInstance();

    // COM on the UI thread (file dialogs), and process-wide security that lets the
    // System Restore service call back into us, as SRSetRestorePoint requires.
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    {
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
            RPC_C_IMP_LEVEL_IDENTIFY, nullptr, EOAC_NONE, nullptr);
    }

    CMainDlg dialog;
    dialog.InstallOnStart(UpdateIdsFromCommandLine());
    m_pMainWnd = &dialog;
    dialog.DoModal();

    CoUninitialize();

    // The dialog has closed; end the application instead of starting a message pump.
    return FALSE;
}
