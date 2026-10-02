// Windows: IFileOpenDialog. See file_picker.h.
#include "file_picker.h"

#include <windows.h>
#include <shobjidl.h>

namespace bots::setup {

std::filesystem::path PickIsoFile() {
  std::filesystem::path result;
  HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  IFileOpenDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) {
    COMDLG_FILTERSPEC filters[] = {{L"Xbox 360 disc image (*.iso)", L"*.iso"},
                                   {L"All files", L"*.*"}};
    dialog->SetFileTypes(2, filters);
    dialog->SetTitle(L"Choose your 50 Cent: Blood on the Sand disc image");
    if (SUCCEEDED(dialog->Show(GetActiveWindow()))) {
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

}  // namespace bots::setup
