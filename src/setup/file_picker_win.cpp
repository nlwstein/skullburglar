// Windows: IFileOpenDialog. See file_picker.h.
#include "file_picker.h"

#include <windows.h>
#include <shobjidl.h>

namespace bots::setup {

namespace {

// `folder`: pick a folder instead of a file.
std::filesystem::path Pick(const wchar_t* title, bool folder) {
  std::filesystem::path result;
  HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  IFileOpenDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) {
    if (folder) {
      FILEOPENDIALOGOPTIONS options = 0;
      if (SUCCEEDED(dialog->GetOptions(&options))) dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    } else {
      COMDLG_FILTERSPEC filters[] = {{L"Xbox 360 disc image (*.iso)", L"*.iso"},
                                     {L"All files", L"*.*"}};
      dialog->SetFileTypes(2, filters);
    }
    dialog->SetTitle(title);
    // Owned by the game window, so it opens in front of it and the window waits for it. GetActiveWindow() alone
    // gave no owner when the launcher wasn't the active window, and the dialog then opened behind it.
    HWND owner = static_cast<HWND>(CurrentPickerHost().owner);
    if (!owner) owner = GetActiveWindow();
    if (owner) SetForegroundWindow(owner);
    if (SUCCEEDED(dialog->Show(owner))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
          result = path;
          CoTaskMemFree(path);
        }
        item->Release();
      }
    }
    dialog->Release();
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return result;
}

}  // namespace

std::filesystem::path PickIsoFile() { return Pick(L"Choose your 50 Cent: Blood on the Sand disc image", false); }

std::filesystem::path PickFolder() { return Pick(L"Choose a folder for the game files", true); }

}  // namespace bots::setup
