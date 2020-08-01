// SecureUxTheme - A secure boot compatible in-memory UxTheme patcher
// Copyright (C) 2020  namazso
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
#include "pch.h"

#include "MainDialog.h"

#include "main.h"
#include "signature.h"
#include "utl.h"

extern "C" NTSYSAPI VOID NTAPI RtlGetNtVersionNumbers(
  _Out_opt_ PULONG NtMajorVersion,
  _Out_opt_ PULONG NtMinorVersion,
  _Out_opt_ PULONG NtBuildNumber
);

extern "C" NTSYSAPI NTSTATUS NTAPI RtlAdjustPrivilege(
  _In_ ULONG Privilege,
  _In_ BOOLEAN Enable,
  _In_ BOOLEAN Client,
  _Out_ PBOOLEAN WasEnabled
);

#define FLG_APPLICATION_VERIFIER (0x100)

static const wchar_t* PatcherStateText(PatcherState state)
{
  static const wchar_t* const text[] = { L"No", L"Yes", L"Probably", L"Outdated" };
  return text[(size_t)state];
}

static constexpr wchar_t kPatcherDllName[] = L"SecureUxTheme.dll";
static constexpr wchar_t kIFEO[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\";
static constexpr wchar_t kHelpText[] =
LR"(- For any custom themes to work SecureUxTheme or another patcher must be installed
- Styles need to be signed, the signature just doesn't need to be valid
  - To add an invalid signature to a style click "Fix signature"
  - Alternatively, you can simply drag && drop files onto this window.
  - ThemeTool can automatically add them when applying.
- After install and reboot, there are multiple ways to set themes:
  - Hooking SystemSettings, patching themes, then Settings (1703+)
  - Patching themes and clicking "Personalization" to start a hooked instance
  - Using ThemeTool to apply themes.
)";

static std::wstring GetPatcherDllPath()
{
  std::wstring path;
  const auto status = utl::get_KnownDllPath(path);
  if (status != NO_ERROR)
    utl::Fatal(nullptr, L"Cannot find KnownDllPath %08X", status);

  path += L"\\";
  path += kPatcherDllName;
  return path;
}

static int WinlogonBypassCount()
{
  return utl::atom_reference_count(L"SecureUxTheme_CalledInWinlogon");
}

std::wstring GetWindowTextStr(HWND hwnd)
{
  SetLastError(0);
  const auto len = GetWindowTextLengthW(hwnd);
  const auto error = GetLastError();
  if (len == 0 && error != 0)
    return {};
  std::wstring str;
  str.resize(len + 1);
  str.resize(GetWindowTextW(hwnd, str.data(), str.size()));
  return str;
}

void MainDialog::Log(const wchar_t* fmt, ...)
{
  std::wstring str;
  va_list args;
  va_start(args, fmt);
  utl::vfmt(str, fmt, args);
  va_end(args);
  auto log = GetWindowTextStr(_hwnd_LOG);
  if(!log.empty())
    log.append(L"\r\n");
  LARGE_INTEGER li{};
  QueryPerformanceCounter(&li);
  log.append(std::to_wstring(li.QuadPart));
  log.append(L" > ");
  log.append(str.c_str());
  SetWindowTextW(_hwnd_LOG, log.c_str());
}

bool MainDialog::IsInstalledForExecutable(const wchar_t* executable)
{
  const auto subkey = std::wstring{ kIFEO } +executable;
  DWORD GlobalFlag = 0;
  DWORD GlobalFlag_size = sizeof(GlobalFlag);
  const auto ret1 = RegGetValueW(
    HKEY_LOCAL_MACHINE,
    subkey.c_str(),
    L"GlobalFlag",
    RRF_RT_REG_DWORD | RRF_ZEROONFAILURE,
    nullptr,
    &GlobalFlag,
    &GlobalFlag_size
  );
  wchar_t VerifierDlls[257];
  DWORD VerifierDlls_size = sizeof(VerifierDlls);
  const auto ret2 = RegGetValueW(
    HKEY_LOCAL_MACHINE,
    subkey.c_str(),
    L"VerifierDlls",
    RRF_RT_REG_SZ | RRF_ZEROONFAILURE,
    nullptr,
    VerifierDlls,
    &VerifierDlls_size
  );
  return GlobalFlag & FLG_APPLICATION_VERIFIER && 0 == _wcsicmp(VerifierDlls, kPatcherDllName);
}

DWORD MainDialog::InstallForExecutable(const wchar_t* executable)
{
  const auto subkey = std::wstring{ kIFEO } +executable;
  DWORD GlobalFlag = 0;
  DWORD GlobalFlag_size = sizeof(GlobalFlag);
  // we don't care if it fails
  RegGetValueW(
    HKEY_LOCAL_MACHINE,
    subkey.c_str(),
    L"GlobalFlag",
    RRF_RT_REG_DWORD | RRF_ZEROONFAILURE,
    nullptr,
    &GlobalFlag,
    &GlobalFlag_size
  );
  GlobalFlag |= FLG_APPLICATION_VERIFIER;
  auto ret = RegSetKeyValueW(
    HKEY_LOCAL_MACHINE,
    subkey.c_str(),
    L"GlobalFlag",
    REG_DWORD,
    &GlobalFlag,
    sizeof(GlobalFlag)
  );
  if(!ret)
  {
    ret = RegSetKeyValueW(
      HKEY_LOCAL_MACHINE,
      subkey.c_str(),
      L"VerifierDlls",
      REG_SZ,
      kPatcherDllName,
      sizeof(kPatcherDllName)
    );
  }
  return ret;
}

DWORD MainDialog::UninstallForExecutable(const wchar_t* executable)
{
  const auto subkey = std::wstring{ kIFEO } +executable;
  DWORD GlobalFlag = 0;
  DWORD GlobalFlag_size = sizeof(GlobalFlag);
  // we don't care if it fails
  RegGetValueW(
    HKEY_LOCAL_MACHINE,
    subkey.c_str(),
    L"GlobalFlag",
    RRF_RT_REG_DWORD | RRF_ZEROONFAILURE,
    nullptr,
    &GlobalFlag,
    &GlobalFlag_size
  );
  GlobalFlag &= ~FLG_APPLICATION_VERIFIER;
  DWORD ret = ERROR_SUCCESS;
  if(!GlobalFlag)
  {
    ret = RegDeleteKeyValueW(
      HKEY_LOCAL_MACHINE,
      subkey.c_str(),
      L"GlobalFlag"
    );
  }
  else
  {
    ret = RegSetKeyValueW(
      HKEY_LOCAL_MACHINE,
      subkey.c_str(),
      L"GlobalFlag",
      REG_DWORD,
      &GlobalFlag,
      sizeof(GlobalFlag)
    );
  }

  // query it again, so we don't delete the other key if we failed removing the flag somehow, that would login loop
  GlobalFlag_size = sizeof(GlobalFlag);
  RegGetValueW(
    HKEY_LOCAL_MACHINE,
    subkey.c_str(),
    L"GlobalFlag",
    RRF_RT_REG_DWORD | RRF_ZEROONFAILURE,
    nullptr,
    &GlobalFlag,
    &GlobalFlag_size
  );
  if(!(GlobalFlag & FLG_APPLICATION_VERIFIER))
  {
    // FLG_APPLICATION_VERIFIER is not set, we don't care how we got here, nor if we succeed deleting VerifierDlls
    ret = ERROR_SUCCESS;

    RegDeleteKeyValueW(
      HKEY_LOCAL_MACHINE,
      subkey.c_str(),
      L"VerifierDlls"
    );
  }

  return ret;
}

DWORD MainDialog::UninstallInternal()
{
  Log(L"Uninstall started...");

  static const wchar_t* remove_from[] = {
    L"winlogon.exe",
    L"explorer.exe",
    L"SystemSettings.exe",
    L"dwm.exe",
    L"LogonUI.exe"
  };

  DWORD ret = ERROR_SUCCESS;
  auto failed = false;

  for (const auto executable : remove_from)
  {
    ret = UninstallForExecutable(executable);
    Log(L"UninstallForExecutable(\"%s\") returned %08X", executable, ret);
    failed = ret != 0;
    if (failed)
      break;
  }

  if (failed)
  {
    utl::FormattedMessageBox(
      _hwnd,
      L"Error",
      MB_OK | MB_ICONERROR,
      L"Uninstalling failed, see log for more info. Error: %s",
      utl::ErrorToString(ret).c_str()
    );
    return ret;
  }

  const auto dll_path = GetPatcherDllPath();
  ret = utl::nuke_file(dll_path);
  Log(L"utl::nuke_file returned: %08X", ret);
  if (ret)
  {
    utl::FormattedMessageBox(
      _hwnd,
      L"Warning",
      MB_OK | MB_ICONWARNING,
      L"Uninstalling succeeded, but the file couldn't be removed. This may cause problems on reinstall. Error: %s",
      utl::ErrorToString(ret).c_str()
    );
  }
  return ret;
}

void MainDialog::Uninstall()
{
  {
    utl::unique_redirection_disabler disabler{};

    // TODO: warn user if current theme not signed

    UninstallInternal();
  }

  UpdatePatcherState();
}

void MainDialog::Install()
{
  utl::unique_redirection_disabler disabler{};

  auto ret = UninstallInternal();

  if(ret)
  {
    utl::FormattedMessageBox(
      _hwnd,
      L"Error",
      MB_OK | MB_ICONERROR,
      L"Installation cannot continue because uninstalling failed"
    );
    return;
  }

  Log(L"Install started...");

  const auto dll_path = GetPatcherDllPath();
  const auto blob = utl::get_dll_blob();
  ret = utl::write_file(dll_path, blob.first, blob.second);
  Log(L"utl::write_file returned %08X", ret);
  if(ret)
  {
    utl::FormattedMessageBox(
      _hwnd,
      L"Error",
      MB_OK | MB_ICONERROR,
      L"Installing patcher DLL failed. Error: %s",
      utl::ErrorToString(ret)
    );
    return;
  }

  ret = InstallForExecutable(L"winlogon.exe");
  Log(L"InstallForExecutable(\"winlogon.exe\") returned %08X", ret);
  if(ret)
  {
    utl::FormattedMessageBox(
      _hwnd,
      L"Error",
      MB_OK | MB_ICONERROR,
      L"Installing main hook failed. Error: %s",
      utl::ErrorToString(ret).c_str()
    );
    UninstallInternal();
    return;
  }

  static constexpr std::pair<HWND MainDialog::*, const wchar_t*> checks[]
  {
    { &MainDialog::_hwnd_CHECK_EXPLORER,       L"explorer.exe"       },
    { &MainDialog::_hwnd_CHECK_LOGONUI,        L"LogonUI.exe"        },
    { &MainDialog::_hwnd_CHECK_SYSTEMSETTINGS, L"SystemSettings.exe" },
  };

  for(const auto& check : checks)
  {
    if (BST_CHECKED != Button_GetCheck(this->*check.first))
      continue;

    const auto ret = InstallForExecutable(check.second);
    Log(L"InstallForExecutable(\"%s\") returned %08X", check.second, ret);
    if(ret)
    {
      utl::FormattedMessageBox(
        _hwnd,
        L"Warning",
        MB_OK | MB_ICONWARNING,
        L"Installing for \"%s\" failed. Error: %s",
        check.second,
        utl::ErrorToString(ret).c_str()
      );
    }
  }

  const auto reboot = IDYES == utl::FormattedMessageBox(
    _hwnd,
    L"Success",
    MB_YESNO,
    L"Installing succeeded, patcher will be loaded next boot. Do you want to reboot now or later?"
  );

  if(reboot)
  {
    BOOLEAN old = FALSE;
    const auto status = RtlAdjustPrivilege(19, TRUE, FALSE, &old);
    Log(L"RtlAdjustPrivilege returned %08X", status);
    if(!NT_SUCCESS(status))
    {
      utl::FormattedMessageBox(
        _hwnd,
        L"Error",
        MB_OK | MB_ICONERROR,
        L"Adjusting shutdown privilege failed. Error: %s",
        utl::ErrorToString(RtlNtStatusToDosError(status)).c_str()
      );
      return;
    }

    const auto succeeded = ExitWindowsEx(EWX_REBOOT, 0);
    if(!succeeded)
    {
      ret = GetLastError();
      Log(L"ExitWindowsEx failed with GetLastError() = %08X", ret);
      utl::FormattedMessageBox(
        _hwnd,
        L"Error",
        MB_OK | MB_ICONERROR,
        L"Rebooting failed. Error: %s",
        utl::ErrorToString(ret).c_str()
      );
    }
  }
}

void MainDialog::UpdatePatcherState()
{
  utl::unique_redirection_disabler d{};
  const auto dll_path = GetPatcherDllPath();
  const auto dll_expected_content = utl::get_dll_blob();
  bool file_has_content;
  bool file_is_same;
  DWORD file_error;

  {
    std::vector<char> content;
    file_error = utl::read_file(dll_path, content);
    file_has_content = !content.empty();
    const auto begin = (char*)dll_expected_content.first;
    const auto end = begin + dll_expected_content.second;
    file_is_same = std::equal(content.begin(), content.end(), begin, end);
  }

  const auto reg_winlogon = IsInstalledForExecutable(L"winlogon.exe");
  const auto reg_explorer = IsInstalledForExecutable(L"explorer.exe");
  const auto reg_systemsettings = IsInstalledForExecutable(L"SystemSettings.exe");
  const auto reg_logonui = IsInstalledForExecutable(L"LogonUI.exe");
  const auto bypass_count = WinlogonBypassCount();
  Log(
    L"UpdatePatcherState: file_has_content %d file_is_same %d file_error %d bypass_count %d",
    file_has_content, file_is_same, file_error, bypass_count
  );
  _is_installed =
    (file_has_content && reg_winlogon)
    ? (file_is_same ? PatcherState::Yes : PatcherState::Outdated)
    : PatcherState::No;
  _is_loaded =
    bypass_count > 0
    ? PatcherState::Yes
    : (_is_installed == PatcherState::Outdated ? PatcherState::Probably : PatcherState::No);
  _is_logonui = reg_logonui ? PatcherState::Yes : PatcherState::No;
  _is_explorer = reg_explorer ? PatcherState::Yes : PatcherState::No;
  _is_systemsettings = reg_systemsettings ? PatcherState::Yes : PatcherState::No;

  UpdatePatcherStateDisplay();
}

void MainDialog::UpdatePatcherStateDisplay()
{
  static constexpr std::pair<PatcherState MainDialog::*, HWND MainDialog::*> statics[] 
  {
    { &MainDialog::_is_installed,       &MainDialog::_hwnd_STATIC_INSTALLED       },
    { &MainDialog::_is_loaded,          &MainDialog::_hwnd_STATIC_LOADED          },
    { &MainDialog::_is_logonui,         &MainDialog::_hwnd_STATIC_LOGONUI         },
    { &MainDialog::_is_explorer,        &MainDialog::_hwnd_STATIC_EXPLORER        },
    { &MainDialog::_is_systemsettings,  &MainDialog::_hwnd_STATIC_SYSTEMSETTINGS  },
  };
  for (const auto& x : statics)
    SetWindowTextW(this->*x.second, PatcherStateText(this->*x.first));
}

MainDialog::MainDialog(HWND hDlg, void*)
  : _hwnd(hDlg)
{
  ULONG major = 0, minor = 0, build = 0;
  RtlGetNtVersionNumbers(&major, &minor, &build);
  Log(L"Running on %d.%d.%d flavor %01X", major, minor, build & 0xFFFF, build >> 28);
  
  Log(L"MainDialog: is_elevated %d", _is_elevated);

  Log(L"Session user: %s Process user: %s", _session_user.second.c_str(), _process_user.second.c_str());

  Static_SetText(_hwnd_STATIC_ASADMIN, PatcherStateText(_is_elevated ? PatcherState::Yes : PatcherState::No));

  if(!_is_elevated)
  {
    Button_Enable(_hwnd_BUTTON_INSTALL, FALSE);
    Button_Enable(_hwnd_BUTTON_UNINSTALL, FALSE);
  }

  ListView_SetExtendedListViewStyle(_hwnd_LIST, LVS_EX_AUTOSIZECOLUMNS | LVS_EX_FULLROWSELECT);
  LVCOLUMN col{};
  ListView_InsertColumn(_hwnd_LIST, 0, &col);
  SendMessage(_hwnd_LIST, LVM_SETTEXTBKCOLOR, 0, (LPARAM)CLR_NONE);

  //auto style = GetWindowStyle(_hwnd_LIST);
  //style |= LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | LVS_ALIGNLEFT | LVS_NOCOLUMNHEADER;
  //SetWindowLongW(_hwnd_LIST, GWL_STYLE, style);

  int iCount = 0;
  g_pThemeManager2->GetThemeCount(&iCount);

  //int iCurrent = 0;
  //g_pThemeManager2->GetCurrentTheme(&iCurrent);

  const auto add_item = [this](LPCTSTR name, LPARAM lparam)
  {
    LVITEM lvitem
    {
      LVIF_PARAM,
      INT_MAX,
      0,
      0,
      0,
      (LPTSTR)_T("")
    };
    lvitem.lParam = lparam;
    const auto item = ListView_InsertItem(_hwnd_LIST, &lvitem);
    ListView_SetItemText(_hwnd_LIST, item, 0, (LPTSTR)name);
  };

  for (auto i = 0; i < iCount; ++i)
  {
    ITheme* pTheme = nullptr;
    g_pThemeManager2->GetTheme(i, &pTheme);

    const auto name = pTheme->GetDisplayName();

    add_item(name.c_str(), i);

    pTheme->Release();
  }

  // LVS_EX_AUTOSIZECOLUMNS just doesn't fucking work no matter where I put it
  ListView_SetColumnWidth(_hwnd_LIST, 0, LVSCW_AUTOSIZE);

  UpdatePatcherState();
}

/*void MainDialog::SelectTheme(int id)
{
  if (id == -1)
  {
    Static_SetText(_hwnd_STATIC_STYLE, _T("Error: Invalid selection"));
    return;
  }

  ITheme* pTheme = nullptr;
  g_pThemeManager2->GetTheme(id, &pTheme);

  const auto style = pTheme->GetVisualStyle();

  Static_SetText(_hwnd_STATIC_STYLE, style);

  if(wcslen(style) > 1 && sig::check_file(style) == E_FAIL)
  {
    Static_SetText(_hwnd_STATIC_NEEDS_PATCH, _T("Style needs patching"));
    Button_SetText(_hwnd_BUTTON_APPLY, _T("Patch and apply"));
  }
  else
  {
    Static_SetText(_hwnd_STATIC_NEEDS_PATCH, _T(""));
    Button_SetText(_hwnd_BUTTON_APPLY, _T("Apply"));
  }

  pTheme->Release();
}*/

void MainDialog::ApplyTheme(int id)
{
  Log(L"ApplyTheme(%d)", id);

  if (id == -1)
    return; // invalid selection... whatever..

  if(_session_user != _process_user)
  {
    const auto answer = utl::FormattedMessageBox(
      _hwnd,
      L"Warning",
      MB_YESNO | MB_ICONWARNING,
      LR"(This program is running as "%s", but you're logged in as "%s".
Setting a theme will apply it to user "%s".
Please note that setting a theme can be done as a non-administrator account.
Are you sure you want to continue?)",
      _process_user.second.c_str(),
      _session_user.second.c_str(),
      _process_user.second.c_str()
    );

    if (answer == IDNO)
      return;
  }

  bool patched = true;
  std::wstring style;

  {
    CComPtr<ITheme> pTheme = nullptr;
    auto result = g_pThemeManager2->GetTheme(id, &pTheme);
    if(SUCCEEDED(result))
    {
      result = pTheme->GetVisualStyle(style);
      if (SUCCEEDED(result))
      {
        if (!style.empty() && sig::check_file(style.c_str()) == E_FAIL)
          patched = false;
      }
      else
      {
        Log(L"pTheme->GetVisualStyle failed with %08X", result);
      }
    }
    else
    {
      Log(L"g_pThemeManager2->GetTheme(%d) failed with %08X", id, result);
      return;
    }
  }

  Log(L"Style path is %s", style.c_str());

  if(_is_installed != PatcherState::No)
  {
    HRESULT fix_result = NOERROR;
    if (!patched)
    {
      fix_result = sig::fix_file(style.c_str());
      patched = SUCCEEDED(fix_result);
    }

    if(!patched)
    {
      Log(L"sig::fix_file failed: %08X", fix_result);
      const auto answer = utl::FormattedMessageBox(
        _hwnd,
        L"Warning",
        MB_YESNO | MB_ICONWARNING,
        LR"(You seem to be using SecureUxTheme, however the selected theme isn't patched, patching it now failed.
%s
The error encountered was: %s.
Do you want to continue?)",
        _is_elevated
          ? L"Try executing the tool as administrator."
          : L"It seems like we're already elevated. Consider submitting a but report.",
        utl::ErrorToString(fix_result).c_str()
      );

      if (answer == IDNO)
        return;
    }

    if(_is_installed == PatcherState::Yes && _is_loaded != PatcherState::Yes)
    {
      const auto answer = utl::FormattedMessageBox(
        _hwnd,
        L"Warning",
        MB_YESNO | MB_ICONWARNING,
        LR"(It seems like SecureUxTheme is installed but not loaded. Custom themes likely won't work.
Make sure you didn't forget to restart your computer after installing.
Do you still want to continue?)"
      );

      if (answer == IDNO)
        return;
    }
  }
  else
  {
    if(!patched)
    {
      const auto answer = utl::FormattedMessageBox(
        _hwnd,
        L"Warning",
        MB_YESNO | MB_ICONWARNING,
        LR"(You seem not to be using SecureUxTheme, and trying to apply an unsigned theme.
This won't work unless another patcher is installed.
Are you sure you want to continue?)"
      );

      if (answer == IDNO)
        return;
    }
  }

  auto apply_flags = 0;
  
#define CHECK_FLAG(flag) apply_flags |= Button_GetCheck(_hwnd_CHECK_ ## flag) ? THEME_APPLY_FLAG_ ## flag : 0

  CHECK_FLAG(IGNORE_BACKGROUND);
  CHECK_FLAG(IGNORE_CURSOR);
  CHECK_FLAG(IGNORE_DESKTOP_ICONS);
  CHECK_FLAG(IGNORE_COLOR);
  CHECK_FLAG(IGNORE_SOUND);

#undef CHECK_FLAG

  const auto old_count = WinlogonBypassCount();

  HRESULT result;

  {
    utl::unique_redirection_disabler disabler{};

    result = g_pThemeManager2->SetCurrentTheme(
      _hwnd,
      id,
      1,
      (THEME_APPLY_FLAGS)apply_flags,
      (THEMEPACK_FLAGS)0
    );
  }

  const auto new_count = WinlogonBypassCount();

  Log(L"ApplyTheme: SetCurrentTheme returned %08X atom: %d -> %d", result, old_count, new_count);

  if(FAILED(result))
  {
    utl::FormattedMessageBox(
      _hwnd,
      L"Error",
      MB_OK | MB_ICONERROR,
      L"Theme setting failed. The following error was encountered:\r\n%s\r\nConsider submitting a bug report.",
      utl::ErrorToString(result).c_str()
    );
  }

  // This happens with the default windows theme, no idea why.
  /*else if(new_count <= old_count && _is_loaded == PatcherState::Yes)
  {
    utl::FormattedMessageBox(
      _hwnd,
      L"Error",
      MB_OK | MB_ICONWARNING,
      L"Theme setting reported success, however bypass count didn't increase. Weird."
    );
  }*/
}

int MainDialog::CurrentSelection()
{
  const auto count = ListView_GetSelectedCount(_hwnd_LIST);
  if (count != 1)
  {
    Log(L"CurrentSelection: count is %d, expected 1", count);
    return -1;
  }

  LVITEM item{};
  item.iItem = ListView_GetSelectionMark(_hwnd_LIST);
  item.mask = LVIF_PARAM;
  ListView_GetItem(_hwnd_LIST, &item);
  return (int)item.lParam;
}

INT_PTR MainDialog::DlgProc(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  UNREFERENCED_PARAMETER(lParam);
  switch (uMsg)
  {
  case WM_INITDIALOG:
    return FALSE; // do not select default control

  case WM_COMMAND:
    switch (LOWORD(wParam))
    {
    case IDOK:
    case IDCLOSE:
    case IDCANCEL:
      if (HIWORD(wParam) == BN_CLICKED)
        DestroyWindow(_hwnd);
      return TRUE;
    case IDC_BUTTON_HELP:
      if (HIWORD(wParam) == BN_CLICKED)
        MessageBoxW(_hwnd, kHelpText, L"Help", MB_OK);
      return TRUE;
    case IDC_BUTTON_INSTALL:
      if (HIWORD(wParam) == BN_CLICKED)
        Install();
      return TRUE;
    case IDC_BUTTON_UNINSTALL:
      if (HIWORD(wParam) == BN_CLICKED)
        Uninstall();
      return TRUE;
    /*case IDC_COMBO_THEMES:
      if (HIWORD(wParam) == CBN_SELENDOK)
        SelectTheme(CurrentSelection());
      return TRUE;*/
    case IDC_BUTTON_APPLY:
      if (HIWORD(wParam) == BN_CLICKED)
        ApplyTheme(CurrentSelection());
      return TRUE;
    }
    break;

  case WM_CLOSE:
    DestroyWindow(_hwnd);
    return TRUE;

  case WM_DESTROY:
    PostQuitMessage(0);
    return TRUE;
  }
  return (INT_PTR)FALSE;
}
