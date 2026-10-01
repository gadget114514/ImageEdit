#include "app.h"
#include "editor.h"
#include "image_util.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")

#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

namespace {

enum {
    IDC_BTN_OPEN = 2001, IDC_BTN_ADD, IDC_BTN_EDIT, IDC_BTN_DEL,
    IDC_BTN_REFRESH, IDC_BTN_RELOAD, IDC_LIST, IDM_EXIT = 2100, IDM_EDIT,
    IDM_REVEAL, IDM_REMOVE
};

struct ListItem {
    std::wstring path;
    int width = 0, height = 0;
    double dpiX = 96, dpiY = 96;
    int imageIndex = -1;
};

HINSTANCE g_inst = nullptr;
HWND g_main = nullptr;
HWND g_list = nullptr;
HWND g_status = nullptr;
HWND g_btnOpen = nullptr, g_btnAdd = nullptr, g_btnEdit = nullptr;
HWND g_btnDel = nullptr, g_btnRefresh = nullptr, g_btnReload = nullptr;
HIMAGELIST g_himl = nullptr;
HFONT g_font = nullptr;
HACCEL g_accel = nullptr;
UINT g_dpi = 96;
int g_thumb = 160;
std::vector<ListItem> g_items;
std::vector<std::wstring> g_folders;
std::wstring g_lastFolder;

const wchar_t* kRegPath = L"Software\\ImageEdit";

int Sc(int v) { return MulDiv(v, (int)g_dpi, 96); }

std::wstring FileNameOf(const std::wstring& p)
{
    return fs::path(p).filename().wstring();
}

int FindItemByPath(const std::wstring& path)
{
    for (size_t i = 0; i < g_items.size(); i++)
        if (_wcsicmp(g_items[i].path.c_str(), path.c_str()) == 0)
            return (int)i;
    return -1;
}

int FindFolder(const std::wstring& dir)
{
    for (size_t i = 0; i < g_folders.size(); i++)
        if (_wcsicmp(g_folders[i].c_str(), dir.c_str()) == 0)
            return (int)i;
    return -1;
}

void AddFolderToList(const std::wstring& dir)
{
    if (dir.empty())
        return;
    std::wstring d = dir;
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/'))
        d.pop_back();
    if (FindFolder(d) < 0)
        g_folders.push_back(d);
}

void EnsureFoldersFromItems()
{
    for (const ListItem& it : g_items)
    {
        std::wstring parent = fs::path(it.path).parent_path().wstring();
        if (!parent.empty())
            AddFolderToList(parent);
    }
}

std::wstring RegGetString(const wchar_t* name)
{
    wchar_t buf[2048] = L"";
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, name, RRF_RT_REG_SZ,
                     nullptr, buf, &size) != ERROR_SUCCESS)
        return L"";
    return buf;
}

std::vector<std::wstring> RegGetMultiSz(const wchar_t* name)
{
    std::vector<std::wstring> out;
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, name, RRF_RT_REG_MULTI_SZ,
                     nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0)
        return out;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, name, RRF_RT_REG_MULTI_SZ,
                     nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return out;
    const wchar_t* p = buf.data();
    while (*p)
    {
        out.push_back(p);
        p += wcslen(p) + 1;
    }
    return out;
}

void SaveState()
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, L"LastFolder", 0, REG_SZ,
                   (const BYTE*)g_lastFolder.c_str(),
                   (DWORD)((g_lastFolder.size() + 1) * sizeof(wchar_t)));

    std::vector<wchar_t> multi;
    for (const ListItem& it : g_items)
    {
        multi.insert(multi.end(), it.path.begin(), it.path.end());
        multi.push_back(L'\0');
    }
    multi.push_back(L'\0');
    RegSetValueExW(key, L"Items", 0, REG_MULTI_SZ,
                   (const BYTE*)multi.data(),
                   (DWORD)(multi.size() * sizeof(wchar_t)));

    std::vector<wchar_t> folders;
    for (const std::wstring& d : g_folders)
    {
        folders.insert(folders.end(), d.begin(), d.end());
        folders.push_back(L'\0');
    }
    folders.push_back(L'\0');
    RegSetValueExW(key, L"Folders", 0, REG_MULTI_SZ,
                   (const BYTE*)folders.data(),
                   (DWORD)(folders.size() * sizeof(wchar_t)));
    RegCloseKey(key);
}

void SetDialogFolder(IFileOpenDialog* dlg, const std::wstring& dir)
{
    if (dir.empty())
        return;
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr,
                                              IID_PPV_ARGS(&item))))
    {
        dlg->SetFolder(item);
        item->Release();
    }
}

void SetStatusCount()
{
    wchar_t b[64];
    swprintf_s(b, L"%d 件", (int)g_items.size());
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)b);
}

void SelectItemByIndex(int itemIndex)
{
    int n = ListView_GetItemCount(g_list);
    for (int i = 0; i < n; i++)
    {
        LVITEMW lvi{};
        lvi.mask = LVIF_PARAM;
        lvi.iItem = i;
        if (ListView_GetItem(g_list, &lvi) && (int)lvi.lParam == itemIndex)
        {
            ListView_SetItemState(g_list, i, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(g_list, i, FALSE);
            break;
        }
    }
}

int GetSelectedItemIndex()
{
    int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (sel < 0)
        return -1;
    LVITEMW lvi{};
    lvi.mask = LVIF_PARAM;
    lvi.iItem = sel;
    if (!ListView_GetItem(g_list, &lvi))
        return -1;
    int idx = (int)lvi.lParam;
    if (idx < 0 || idx >= (int)g_items.size())
        return -1;
    return idx;
}

void MainAddFile(const std::wstring& path)
{
    int exist = FindItemByPath(path);
    if (exist >= 0)
    {
        SelectItemByIndex(exist);
        return;
    }
    if (!ImgUtil_IsSupportedFile(path))
        return;

    int w = 0, h = 0;
    double dx = 96, dy = 96;
    if (!ImgUtil_LoadInfo(path, &w, &h, &dx, &dy))
        return;

    HBITMAP hbm = ImgUtil_CreateThumbnail(path, g_thumb, g_thumb, nullptr, nullptr);
    if (!hbm)
        return;
    int imgIdx = ImageList_Add(g_himl, hbm, nullptr);
    DeleteObject(hbm);
    if (imgIdx < 0)
        return;

    ListItem item;
    item.path = path;
    item.width = w;
    item.height = h;
    item.dpiX = dx;
    item.dpiY = dy;
    item.imageIndex = imgIdx;
    g_items.push_back(item);

    std::wstring name = FileNameOf(path);
    LVITEMW lvi{};
    lvi.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
    lvi.iItem = ListView_GetItemCount(g_list);
    lvi.iImage = imgIdx;
    lvi.pszText = const_cast<LPWSTR>(name.c_str());
    lvi.lParam = (LPARAM)(g_items.size() - 1);
    ListView_InsertItem(g_list, &lvi);
    SetStatusCount();
}

void MainAddFolder(const std::wstring& dir)
{
    g_lastFolder = dir;
    AddFolderToList(dir);
    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    std::vector<std::wstring> files;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (ImgUtil_IsSupportedFile(full))
            files.push_back(full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(files.begin(), files.end(),
              [](const std::wstring& a, const std::wstring& b)
              { return _wcsicmp(a.c_str(), b.c_str()) < 0; });

    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    for (const auto& f : files)
        MainAddFile(f);
    SetCursor(old);
    SaveState();
}

void RebuildList()
{
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    for (size_t i = 0; i < g_items.size(); i++)
    {
        std::wstring name = FileNameOf(g_items[i].path);
        LVITEMW lvi{};
        lvi.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        lvi.iItem = (int)i;
        lvi.iImage = g_items[i].imageIndex;
        lvi.pszText = const_cast<LPWSTR>(name.c_str());
        lvi.lParam = (LPARAM)i;
        ListView_InsertItem(g_list, &lvi);
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);
    SetStatusCount();
}

void MainRemoveSelected()
{
    std::vector<int> sel;
    int i = -1;
    while ((i = ListView_GetNextItem(g_list, i, LVNI_SELECTED)) >= 0)
    {
        LVITEMW lvi{};
        lvi.mask = LVIF_PARAM;
        lvi.iItem = i;
        if (ListView_GetItem(g_list, &lvi))
            sel.push_back((int)lvi.lParam);
    }
    if (sel.empty())
        return;

    std::sort(sel.begin(), sel.end(), std::greater<int>());
    for (int idx : sel)
        if (idx >= 0 && idx < (int)g_items.size())
            g_items.erase(g_items.begin() + idx);

    RebuildList();
    SaveState();
}

void MainRefresh()
{
    if (g_items.empty())
        return;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    for (auto& it : g_items)
    {
        int w = 0, h = 0;
        double dx = 96, dy = 96;
        if (!ImgUtil_LoadInfo(it.path, &w, &h, &dx, &dy))
            continue;
        HBITMAP hbm = ImgUtil_CreateThumbnail(it.path, g_thumb, g_thumb,
                                              nullptr, nullptr);
        if (hbm)
        {
            ImageList_Replace(g_himl, it.imageIndex, hbm, nullptr);
            DeleteObject(hbm);
        }
        it.width = w;
        it.height = h;
        it.dpiX = dx;
        it.dpiY = dy;
    }
    SetCursor(old);
    InvalidateRect(g_list, nullptr, TRUE);
}

void MainReloadFolders()
{
    if (g_folders.empty())
        EnsureFoldersFromItems();
    if (g_folders.empty())
    {
        MessageBoxW(g_main,
                    L"読み込み済みのフォルダがありません。\n"
                    L"「イメージフォルダを開く」でフォルダを開いてください。",
                    L"ImageEdit", MB_ICONINFORMATION);
        return;
    }

    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    g_items.clear();
    if (g_himl)
    {
        ImageList_Destroy(g_himl);
        g_himl = ImageList_Create(g_thumb, g_thumb, ILC_COLOR32, 16, 16);
        ListView_SetImageList(g_list, g_himl, LVSIL_NORMAL);
    }

    std::vector<std::wstring> folders = g_folders;
    for (const std::wstring& d : folders)
    {
        DWORD attr = GetFileAttributesW(d.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        MainAddFolder(d);
    }

    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);
    SetStatusCount();
    SaveState();
    SetCursor(old);
}

void MainOpenEditor()
{
    int idx = GetSelectedItemIndex();
    if (idx < 0)
    {
        MessageBoxW(g_main, L"編集する画像を選択してください。", L"ImageEdit",
                    MB_ICONINFORMATION);
        return;
    }
    Editor_Open(g_main, g_items[idx].path);
}

std::wstring PickFolder(HWND owner)
{
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
    {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"画像フォルダーを選択");
        SetDialogFolder(dlg, g_lastFolder);
        if (SUCCEEDED(dlg->Show(owner)))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)))
            {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)))
                {
                    result = p;
                    CoTaskMemFree(p);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return result;
}

std::vector<std::wstring> PickFiles(HWND owner)
{
    std::vector<std::wstring> out;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
    {
        COMDLG_FILTERSPEC filters[] = {
            { L"画像ファイル", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff" },
            { L"すべてのファイル", L"*.*" },
        };
        dlg->SetFileTypes(2, filters);
        dlg->SetTitle(L"画像を追加");
        SetDialogFolder(dlg, g_lastFolder);
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST |
                        FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(dlg->Show(owner)))
        {
            IShellItemArray* arr = nullptr;
            if (SUCCEEDED(dlg->GetResults(&arr)))
            {
                DWORD n = 0;
                arr->GetCount(&n);
                for (DWORD i = 0; i < n; i++)
                {
                    IShellItem* it = nullptr;
                    if (SUCCEEDED(arr->GetItemAt(i, &it)))
                    {
                        PWSTR p = nullptr;
                        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)))
                        {
                            out.push_back(p);
                            CoTaskMemFree(p);
                        }
                        it->Release();
                    }
                }
                arr->Release();
            }
        }
        dlg->Release();
    }
    return out;
}

void RevealInExplorer(const std::wstring& path)
{
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(g_main, L"open", L"explorer.exe", args.c_str(), nullptr,
                  SW_SHOWNORMAL);
}

void MainLayout(HWND hwnd)
{
    RECT rc{};
    GetClientRect(hwnd, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sr{};
    GetWindowRect(g_status, &sr);
    int statusH = sr.bottom - sr.top;

    int pad = Sc(6);
    int ch = Sc(26);
    int x = pad, y = pad;
    auto place = [&](HWND h, int w)
    {
        if (h)
            MoveWindow(h, x, y, w, ch, TRUE);
        x += w + Sc(6);
    };
    place(g_btnOpen, Sc(150));
    place(g_btnAdd, Sc(120));
    place(g_btnEdit, Sc(90));
    place(g_btnDel, Sc(90));
    place(g_btnRefresh, Sc(100));
    place(g_btnReload, Sc(110));

    int listTop = y + ch + pad;
    MoveWindow(g_list, 0, listTop, rc.right,
               std::max(0, (int)rc.bottom - listTop - statusH), TRUE);

    int parts[2] = { std::max(0, (int)rc.right - Sc(360)), -1 };
    SendMessageW(g_status, SB_SETPARTS, 2, (LPARAM)parts);
}

void CreateMainControls(HWND hwnd)
{
    g_btnOpen = CreateWindowExW(0, WC_BUTTONW, L"イメージフォルダを開く",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10,
        hwnd, (HMENU)IDC_BTN_OPEN, g_inst, nullptr);
    g_btnAdd = CreateWindowExW(0, WC_BUTTONW, L"ファイルを追加",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10,
        hwnd, (HMENU)IDC_BTN_ADD, g_inst, nullptr);
    g_btnEdit = CreateWindowExW(0, WC_BUTTONW, L"編集",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10,
        hwnd, (HMENU)IDC_BTN_EDIT, g_inst, nullptr);
    g_btnDel = CreateWindowExW(0, WC_BUTTONW, L"一覧から削除",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10,
        hwnd, (HMENU)IDC_BTN_DEL, g_inst, nullptr);
    g_btnRefresh = CreateWindowExW(0, WC_BUTTONW, L"サムネイル更新",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10,
        hwnd, (HMENU)IDC_BTN_REFRESH, g_inst, nullptr);
    g_btnReload = CreateWindowExW(0, WC_BUTTONW, L"フォルダ再読込",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10,
        hwnd, (HMENU)IDC_BTN_RELOAD, g_inst, nullptr);

    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_ICON | LVS_AUTOARRANGE |
        LVS_SHOWSELALWAYS,
        0, 0, 10, 10, hwnd, (HMENU)IDC_LIST, g_inst, nullptr);

    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 10, 10, hwnd,
        nullptr, g_inst, nullptr);

    g_himl = ImageList_Create(g_thumb, g_thumb, ILC_COLOR32, 16, 16);
    ListView_SetImageList(g_list, g_himl, LVSIL_NORMAL);
    ListView_SetExtendedListViewStyle(g_list,
        LVS_EX_LABELTIP | LVS_EX_INFOTIP | LVS_EX_DOUBLEBUFFER);
    ListView_SetIconSpacing(g_list, g_thumb + Sc(24), g_thumb + Sc(52));

    HWND controls[] = { g_btnOpen, g_btnAdd, g_btnEdit, g_btnDel,
                        g_btnRefresh, g_btnReload, g_list, g_status };
    for (HWND h : controls)
        SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);

    SendMessageW(g_status, SB_SETTEXTW, 1,
                 (LPARAM)L"ダブルクリックで編集 / 画像をドラッグ＆ドロップでも追加できます");
}

void BuildMenu(HWND hwnd)
{
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDC_BTN_OPEN, L"イメージフォルダを開く(&O)\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDC_BTN_ADD, L"ファイルを追加(&A)\tCtrl+I");
    AppendMenuW(file, MF_STRING, IDC_BTN_RELOAD, L"フォルダを再読込(&L)\tF6");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"終了(&X)");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"ファイル(&F)");

    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, IDC_BTN_EDIT, L"編集(&E)\tEnter");
    AppendMenuW(edit, MF_STRING, IDM_REVEAL, L"エクスプローラーで表示(&R)");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, IDC_BTN_DEL, L"一覧から削除(&D)\tDel");
    AppendMenuW(edit, MF_STRING, IDC_BTN_REFRESH, L"サムネイル更新(&U)\tF5");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)edit, L"編集(&E)");

    SetMenu(hwnd, menu);

    ACCEL acc[] = {
        { FVIRTKEY | FCONTROL, 'O', IDC_BTN_OPEN },
        { FVIRTKEY | FCONTROL, 'I', IDC_BTN_ADD },
        { FVIRTKEY, VK_F5, IDC_BTN_REFRESH },
        { FVIRTKEY, VK_F6, IDC_BTN_RELOAD },
        { FVIRTKEY, VK_DELETE, IDC_BTN_DEL },
        { FVIRTKEY, VK_RETURN, IDC_BTN_EDIT },
    };
    g_accel = CreateAcceleratorTableW(acc, (int)(sizeof(acc) / sizeof(acc[0])));
}

void ShowListContextMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);
    POINT cp = pt;
    ScreenToClient(g_list, &cp);
    LVHITTESTINFO ht{};
    ht.pt = cp;
    int hit = ListView_HitTest(g_list, &ht);
    if (hit >= 0)
        ListView_SetItemState(g_list, hit, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_EDIT, L"編集(&E)");
    AppendMenuW(m, MF_STRING, IDM_REVEAL, L"エクスプローラーで表示(&R)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_REMOVE, L"一覧から削除(&D)");
    UINT cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0,
                              hwnd, nullptr);
    DestroyMenu(m);

    switch (cmd)
    {
    case IDM_EDIT:
        MainOpenEditor();
        break;
    case IDM_REVEAL:
    {
        int idx = GetSelectedItemIndex();
        if (idx >= 0)
            RevealInExplorer(g_items[idx].path);
        break;
    }
    case IDM_REMOVE:
        MainRemoveSelected();
        break;
    }
}

void HandleDrop(HWND hwnd, HDROP drop)
{
    (void)hwnd;
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::wstring> dirs;
    std::vector<std::wstring> files;
    for (UINT i = 0; i < n; i++)
    {
        wchar_t buf[32768];
        if (DragQueryFileW(drop, i, buf, 32768))
        {
            DWORD attr = GetFileAttributesW(buf);
            if (attr != INVALID_FILE_ATTRIBUTES &&
                (attr & FILE_ATTRIBUTE_DIRECTORY))
                dirs.push_back(buf);
            else
                files.push_back(buf);
        }
    }
    DragFinish(drop);

    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    for (const auto& f : files)
        MainAddFile(f);
    for (const auto& d : dirs)
        MainAddFolder(d);
    SetCursor(old);
    SaveState();
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        g_dpi = GetDpiForWindowSafe(hwnd);
        g_thumb = Sc(160);
        g_font = CreateFontW(-MulDiv(9, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Yu Gothic UI");
        CreateMainControls(hwnd);
        BuildMenu(hwnd);
        DragAcceptFiles(hwnd, TRUE);
        MainLayout(hwnd);
        return 0;

    case WM_SIZE:
        if (g_list)
            MainLayout(hwnd);
        return 0;

    case WM_DPICHANGED:
    {
        g_dpi = HIWORD(wParam);
        g_thumb = Sc(160);
        if (g_font)
            DeleteObject(g_font);
        g_font = CreateFontW(-MulDiv(9, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Yu Gothic UI");
        HWND controls[] = { g_btnOpen, g_btnAdd, g_btnEdit, g_btnDel,
                            g_btnRefresh, g_btnReload, g_list, g_status };
        for (HWND h : controls)
            SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
        RECT* pr = (RECT*)lParam;
        SetWindowPos(hwnd, nullptr, pr->left, pr->top, pr->right - pr->left,
                     pr->bottom - pr->top, SWP_NOZORDER | SWP_NOACTIVATE);
        MainLayout(hwnd);
        return 0;
    }

    case WM_DROPFILES:
        HandleDrop(hwnd, (HDROP)wParam);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_BTN_OPEN:
        {
            std::wstring dir = PickFolder(hwnd);
            if (!dir.empty())
                MainAddFolder(dir);
            return 0;
        }
        case IDC_BTN_ADD:
        {
            for (const auto& f : PickFiles(hwnd))
                MainAddFile(f);
            return 0;
        }
        case IDC_BTN_EDIT:
        case IDM_EDIT:
            MainOpenEditor();
            return 0;
        case IDC_BTN_DEL:
        case IDM_REMOVE:
            MainRemoveSelected();
            return 0;
        case IDC_BTN_REFRESH:
            MainRefresh();
            return 0;
        case IDC_BTN_RELOAD:
            MainReloadFolders();
            return 0;
        case IDM_REVEAL:
        {
            int idx = GetSelectedItemIndex();
            if (idx >= 0)
                RevealInExplorer(g_items[idx].path);
            return 0;
        }
        case IDM_EXIT:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;

    case WM_NOTIFY:
    {
        auto nm = (LPNMHDR)lParam;
        if (nm->idFrom == IDC_LIST)
        {
            switch (nm->code)
            {
            case NM_DBLCLK:
                MainOpenEditor();
                return 0;
            case NM_RCLICK:
                ShowListContextMenu(hwnd);
                return 0;
            case LVN_GETINFOTIP:
            {
                auto tip = (NMLVGETINFOTIP*)lParam;
                LVITEMW lvi{};
                lvi.mask = LVIF_PARAM;
                lvi.iItem = tip->iItem;
                if (ListView_GetItem(g_list, &lvi))
                {
                    int idx = (int)lvi.lParam;
                    if (idx >= 0 && idx < (int)g_items.size())
                    {
                        const ListItem& it = g_items[idx];
                        wchar_t buf[1024];
                        swprintf_s(buf, L"%d × %d px  /  %g × %g dpi\n%s",
                                   it.width, it.height, it.dpiX, it.dpiY,
                                   it.path.c_str());
                        wcsncpy_s(tip->pszText, tip->cchTextMax, buf, _TRUNCATE);
                    }
                }
                return 0;
            }
            }
        }
        break;
    }

    case WM_APP_THUMBS_CHANGED:
        MainRefresh();
        return 0;

    case WM_SETFOCUS:
        SetFocus(g_list);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        SaveState();
        if (g_himl)
        {
            ImageList_Destroy(g_himl);
            g_himl = nullptr;
        }
        if (g_accel)
        {
            DestroyAcceleratorTable(g_accel);
            g_accel = nullptr;
        }
        if (g_font)
        {
            DeleteObject(g_font);
            g_font = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

static void SetProcessDpiAwareness()
{
    typedef BOOL(WINAPI * Fn)(DPI_AWARENESS_CONTEXT);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                      "SetProcessDpiAwarenessContext");
    if (fn)
        fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    SetProcessDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdipToken = 0;
    if (Gdiplus::GdiplusStartup(&gdipToken, &gsi, nullptr) != Gdiplus::Ok)
        return 1;

    g_inst = hInstance;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ImageEditMainWnd";
    wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc))
    {
        Gdiplus::GdiplusShutdown(gdipToken);
        CoUninitialize();
        return 1;
    }

    UINT dpi = GetDpiForSystemSafe();
    g_main = CreateWindowExW(0, L"ImageEditMainWnd", L"ImageEdit - 画像一覧",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             MulDiv(1000, dpi, 96), MulDiv(700, dpi, 96),
                             nullptr, nullptr, hInstance, nullptr);
    if (!g_main)
    {
        Gdiplus::GdiplusShutdown(gdipToken);
        CoUninitialize();
        return 1;
    }

    ShowWindow(g_main, nCmdShow);
    UpdateWindow(g_main);

    g_lastFolder = RegGetString(L"LastFolder");

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool hadArgs = false;
    if (argv)
    {
        for (int i = 1; i < argc; i++)
        {
            DWORD attr = GetFileAttributesW(argv[i]);
            if (attr == INVALID_FILE_ATTRIBUTES)
                continue;
            HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            if (attr & FILE_ATTRIBUTE_DIRECTORY)
                MainAddFolder(argv[i]);
            else
                MainAddFile(argv[i]);
            SetCursor(old);
            hadArgs = true;
        }
        LocalFree(argv);
    }

    if (!hadArgs)
    {
        g_folders = RegGetMultiSz(L"Folders");
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        for (const std::wstring& f : RegGetMultiSz(L"Items"))
        {
            DWORD attr = GetFileAttributesW(f.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES &&
                !(attr & FILE_ATTRIBUTE_DIRECTORY))
                MainAddFile(f);
        }
        if (g_items.empty() && !g_lastFolder.empty())
        {
            DWORD attr = GetFileAttributesW(g_lastFolder.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES &&
                (attr & FILE_ATTRIBUTE_DIRECTORY))
                MainAddFolder(g_lastFolder);
        }
        if (g_folders.empty())
            EnsureFoldersFromItems();
        SetCursor(old);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!TranslateAcceleratorW(g_main, g_accel, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    Gdiplus::GdiplusShutdown(gdipToken);
    CoUninitialize();
    return (int)msg.wParam;
}
