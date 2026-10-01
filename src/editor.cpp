#include "app.h"
#include "editor.h"
#include "image_util.h"

#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace Gdiplus;

namespace {

const float MIN_CROP = 8.0f;

enum DragMode {
    DRAG_NONE = 0, DRAG_MOVE, DRAG_NEW,
    DRAG_TL, DRAG_T, DRAG_TR, DRAG_R, DRAG_BR, DRAG_B, DRAG_BL, DRAG_L,
    DRAG_PAN
};

enum {
    IDC_CBO_RATIO = 1001, IDC_ED_RW, IDC_ED_RH, IDC_CHK_DPIBASIS,
    IDC_BTN_RATIOMAX, IDC_BTN_RESET, IDC_BTN_ZOOMFIT, IDC_BTN_ZOOM100,
    IDC_LBL_COLON,
    IDC_GRP_CROP, IDC_ST_CROP,
    IDC_GRP_OUT, IDC_ED_OUTW, IDC_LBL_OUTX, IDC_ED_OUTH, IDC_LBL_OUTPX,
    IDC_CHK_KEEP, IDC_LBL_PCT, IDC_CBO_PCT,
    IDC_GRP_DPI, IDC_ED_DPIX, IDC_LBL_DPIX, IDC_ED_DPIY, IDC_LBL_DPIY, IDC_BTN_SRCDPI,
    IDC_ST_OUTINFO, IDC_ST_OUTDIR, IDC_BTN_OUTDIR,
    IDC_ST_STATUS, IDC_BTN_SAVE
};

struct RatioPreset { const wchar_t* label; double w, h; bool free; };

const RatioPreset kPresets[] = {
    { L"自由",    0,  0, true  },
    { L"1:1",     1,  1, false },
    { L"4:3",     4,  3, false },
    { L"3:4",     3,  4, false },
    { L"3:2",     3,  2, false },
    { L"2:3",     2,  3, false },
    { L"16:9",   16,  9, false },
    { L"9:16",    9, 16, false },
    { L"16:10",  16, 10, false },
    { L"2:1",     2,  1, false },
    { L"1:2",     1,  2, false },
    { L"カスタム", 0,  0, false },
};
const int PRESET_COUNT = (int)(sizeof(kPresets) / sizeof(kPresets[0]));
const int PRESET_FREE = 0;
const int PRESET_ONE_TO_ONE = 1;
const int PRESET_CUSTOM = PRESET_COUNT - 1;

const double kPercents[] = { 25, 50, 75, 100, 150, 200, 300, 400 };
const int PCT_COUNT = (int)(sizeof(kPercents) / sizeof(kPercents[0]));
const int PCT_DEFAULT = 3;
const int PCT_CUSTOM = PCT_COUNT;

struct EditorState {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    std::unique_ptr<Bitmap> img;
    std::wstring path;
    int imgW = 0, imgH = 0;
    double srcDpiX = 96.0, srcDpiY = 96.0;

    RectF crop;
    double ratioW = 1.0, ratioH = 1.0;
    bool freeRatio = false;
    bool dpiBasis = false;

    double zoom = 1.0;
    PointF origin;
    bool userZoom = false;

    int drag = DRAG_NONE;
    PointF dragStart;
    RectF cropStart;
    PointF panStartOrigin;
    POINT panStartScreen{};

    int dpi = 96;
    HFONT font = nullptr;
    bool updating = false;
    int pctIndex = PCT_DEFAULT;

    int canvasX = 0, canvasY = 0, canvasW = 0, canvasH = 0;

    HWND cboRatio = nullptr, edRW = nullptr, edRH = nullptr, chkDpi = nullptr;
    HWND btnRatioMax = nullptr, btnReset = nullptr;
    HWND btnZoomFit = nullptr, btnZoom100 = nullptr;
    HWND lblColon = nullptr;
    HWND grpCrop = nullptr, stCrop = nullptr;
    HWND grpOut = nullptr, edOutW = nullptr, edOutH = nullptr;
    HWND lblOutX = nullptr, lblOutPx = nullptr;
    HWND chkKeep = nullptr, lblPct = nullptr, cboPct = nullptr;
    HWND grpDpi = nullptr, edDpiX = nullptr, edDpiY = nullptr, btnSrcDpi = nullptr;
    HWND lblDpiX = nullptr, lblDpiY = nullptr;
    HWND stOutInfo = nullptr, stOutDir = nullptr, btnOutDir = nullptr;
    HWND stStatus = nullptr, btnSave = nullptr;
    std::wstring outputDir;
};

EditorState* GetState(HWND hwnd)
{
    return (EditorState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

int Sc(EditorState* st, int v) { return MulDiv(v, st->dpi, 96); }

std::wstring GetText(HWND h)
{
    int n = GetWindowTextLengthW(h);
    if (n <= 0)
        return L"";
    std::wstring s((size_t)n, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    return s;
}

void SetText(HWND h, const std::wstring& s)
{
    SetWindowTextW(h, s.c_str());
}

bool ParseNumber(const std::wstring& text, double* out)
{
    std::wstring s;
    s.reserve(text.size());
    for (wchar_t c : text)
        if (!iswspace(c))
            s.push_back(c);
    if (s.empty())
        return false;
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    if (end == s.c_str() || *end != L'\0')
        return false;
    *out = v;
    return true;
}

std::wstring NumToStr(double v)
{
    wchar_t buf[64];
    if (v == floor(v) && fabs(v) < 1e15)
        swprintf_s(buf, L"%.0f", v);
    else
        swprintf_s(buf, L"%g", v);
    return buf;
}

double RatioValue(EditorState* st)
{
    if (st->freeRatio || st->ratioH <= 0)
        return 0.0;
    double r = st->ratioW / st->ratioH;
    if (st->dpiBasis && st->srcDpiY > 0)
        r *= st->srcDpiX / st->srcDpiY;
    return r;
}

PointF ImgToScreen(EditorState* st, float x, float y)
{
    return PointF(st->origin.X + x * (float)st->zoom,
                  st->origin.Y + y * (float)st->zoom);
}

PointF ScreenToImg(EditorState* st, int x, int y)
{
    return PointF((float)((x - st->origin.X) / st->zoom),
                  (float)((y - st->origin.Y) / st->zoom));
}

bool InCanvas(EditorState* st, int x, int y)
{
    return x >= st->canvasX && y >= st->canvasY &&
           x < st->canvasX + st->canvasW && y < st->canvasY + st->canvasH;
}

void ComputeFit(EditorState* st)
{
    if (!st->img || st->imgW <= 0 || st->imgH <= 0 || st->canvasW <= 0 || st->canvasH <= 0)
        return;
    double zx = (double)st->canvasW / st->imgW;
    double zy = (double)st->canvasH / st->imgH;
    st->zoom = std::min(zx, zy) * 0.97;
    if (st->zoom <= 0.0)
        st->zoom = 1.0;
    st->origin.X = (float)(st->canvasX + (st->canvasW - st->imgW * st->zoom) / 2.0);
    st->origin.Y = (float)(st->canvasY + (st->canvasH - st->imgH * st->zoom) / 2.0);
}

void UpdateInfoTexts(EditorState* st)
{
    if (!st->img)
        return;

    int cw = (int)llround(st->crop.Width);
    int ch = (int)llround(st->crop.Height);
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;

    double mmW = st->srcDpiX > 0 ? cw / st->srcDpiX * 25.4 : 0;
    double mmH = st->srcDpiY > 0 ? ch / st->srcDpiY * 25.4 : 0;

    wchar_t buf[256];
    swprintf_s(buf,
               L"切り抜き: %d × %d px\n物理: %.1f × %.1f mm\n元画像: %d × %d px / %g dpi",
               cw, ch, mmW, mmH, st->imgW, st->imgH, st->srcDpiX);
    SetText(st->stCrop, buf);
}

int RoundCropW(EditorState* st)
{
    int w = (int)llround(st->crop.Width);
    return std::max(1, w);
}

int RoundCropH(EditorState* st)
{
    int h = (int)llround(st->crop.Height);
    return std::max(1, h);
}

void SyncOutSize(EditorState* st)
{
    if (st->pctIndex >= PCT_CUSTOM)
        return;
    double pct = kPercents[st->pctIndex] / 100.0;
    int ow = std::max(1, (int)llround(RoundCropW(st) * pct));
    int oh = std::max(1, (int)llround(RoundCropH(st) * pct));
    wchar_t b[32];
    st->updating = true;
    swprintf_s(b, L"%d", ow);
    SetText(st->edOutW, b);
    swprintf_s(b, L"%d", oh);
    SetText(st->edOutH, b);
    st->updating = false;
}

void UpdateOutPhysicalText(EditorState* st)
{
    double dx = 96, dy = 96;
    if (!ParseNumber(GetText(st->edDpiX), &dx) || dx <= 0)
        dx = st->srcDpiX;
    if (!ParseNumber(GetText(st->edDpiY), &dy) || dy <= 0)
        dy = st->srcDpiY;
    double ow = 0, oh = 0;
    ParseNumber(GetText(st->edOutW), &ow);
    ParseNumber(GetText(st->edOutH), &oh);

    wchar_t buf[256];
    swprintf_s(buf, L"出力: %.0f × %.0f px (%.1f × %.1f mm)",
               ow, oh, ow / dx * 25.4, oh / dy * 25.4);
    SetText(st->stOutInfo, buf);
}

std::wstring GetOutputDir()
{
    wchar_t buf[1024] = L"";
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ImageEdit", L"OutputDir",
                     RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return L"";
    return buf;
}

void SetOutputDir(const std::wstring& dir)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ImageEdit", 0, nullptr,
                        0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        RegSetValueExW(key, L"OutputDir", 0, REG_SZ, (const BYTE*)dir.c_str(),
                       (DWORD)((dir.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

bool GetOutSizePrefs(int* w, int* h)
{
    DWORD wv = 0, hv = 0, size = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ImageEdit", L"OutWidth",
                     RRF_RT_REG_DWORD, nullptr, &wv, &size) != ERROR_SUCCESS)
        return false;
    size = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ImageEdit", L"OutHeight",
                     RRF_RT_REG_DWORD, nullptr, &hv, &size) != ERROR_SUCCESS)
        return false;
    if (wv < 1 || hv < 1 || wv > 100000 || hv > 100000)
        return false;
    *w = (int)wv;
    *h = (int)hv;
    return true;
}

void SaveOutSizePrefs(EditorState* st)
{
    double wd = 0, hd = 0;
    if (!ParseNumber(GetText(st->edOutW), &wd) || wd < 1 ||
        !ParseNumber(GetText(st->edOutH), &hd) || hd < 1)
        return;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ImageEdit", 0, nullptr,
                        0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    DWORD wv = (DWORD)llround(wd);
    DWORD hv = (DWORD)llround(hd);
    RegSetValueExW(key, L"OutWidth", 0, REG_DWORD, (const BYTE*)&wv, sizeof(wv));
    RegSetValueExW(key, L"OutHeight", 0, REG_DWORD, (const BYTE*)&hv, sizeof(hv));
    RegCloseKey(key);
}

std::wstring PickFolderDialog(HWND owner, LPCWSTR title,
                              const std::wstring& initialDir)
{
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
    {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(title);
        if (!initialDir.empty())
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(
                    initialDir.c_str(), nullptr, IID_PPV_ARGS(&item))))
            {
                dlg->SetFolder(item);
                item->Release();
            }
        }
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

void UpdateOutDirText(EditorState* st)
{
    if (st->outputDir.empty())
    {
        SetText(st->stOutDir, L"出力先: (元画像と同じフォルダ)");
        return;
    }
    wchar_t buf[2048];
    wcsncpy_s(buf, st->outputDir.c_str(), _TRUNCATE);
    HDC hdc = GetDC(st->hwnd);
    if (hdc)
    {
        PathCompactPathW(hdc, buf, (UINT)Sc(st, 250));
        ReleaseDC(st->hwnd, hdc);
    }
    std::wstring s = L"出力先: ";
    s += buf;
    SetText(st->stOutDir, s);
}

void EnableRatioControls(EditorState* st)
{
    BOOL en = st->freeRatio ? FALSE : TRUE;
    EnableWindow(st->edRW, en);
    EnableWindow(st->edRH, en);
    EnableWindow(st->chkDpi, en);
}

void ApplyRatioToCrop(EditorState* st, bool enlarge)
{
    if (st->freeRatio || st->imgW <= 0 || st->imgH <= 0)
        return;
    double r = RatioValue(st);
    if (r <= 0)
        return;

    double cx = st->crop.X + st->crop.Width / 2.0;
    double cy = st->crop.Y + st->crop.Height / 2.0;
    double w, h;
    if (enlarge)
    {
        w = std::min((double)st->imgW, st->imgH * r);
        h = w / r;
    }
    else
    {
        w = st->crop.Width;
        h = st->crop.Height;
        if (w <= 0 || h <= 0)
        {
            w = st->imgW;
            h = st->imgH;
        }
        if (w / h > r)
            w = h * r;
        else
            h = w / r;
    }
    if (w > st->imgW)
    {
        w = st->imgW;
        h = w / r;
    }
    if (h > st->imgH)
    {
        h = st->imgH;
        w = h * r;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    double x = cx - w / 2.0;
    double y = cy - h / 2.0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > st->imgW) x = st->imgW - w;
    if (y + h > st->imgH) y = st->imgH - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    st->crop = RectF((REAL)x, (REAL)y, (REAL)w, (REAL)h);
}

void RoundCrop(EditorState* st)
{
    int x = (int)llround(st->crop.X);
    int y = (int)llround(st->crop.Y);
    int w = std::max(1, (int)llround(st->crop.Width));
    int h = std::max(1, (int)llround(st->crop.Height));
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > st->imgW - 1) x = st->imgW - 1;
    if (y > st->imgH - 1) y = st->imgH - 1;
    if (w > st->imgW - x) w = st->imgW - x;
    if (h > st->imgH - y) h = st->imgH - y;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    st->crop = RectF((REAL)x, (REAL)y, (REAL)w, (REAL)h);
}

int HitTestHandle(EditorState* st, int sx, int sy)
{
    PointF tl = ImgToScreen(st, st->crop.X, st->crop.Y);
    PointF br = ImgToScreen(st, st->crop.X + st->crop.Width, st->crop.Y + st->crop.Height);
    int hs = Sc(st, 6);
    float mx = (tl.X + br.X) / 2.0f;
    float my = (tl.Y + br.Y) / 2.0f;

    struct P { int mode; float x, y; };
    const P pts[] = {
        { DRAG_TL, tl.X, tl.Y }, { DRAG_TR, br.X, tl.Y },
        { DRAG_BR, br.X, br.Y }, { DRAG_BL, tl.X, br.Y },
        { DRAG_T, mx, tl.Y }, { DRAG_R, br.X, my },
        { DRAG_B, mx, br.Y }, { DRAG_L, tl.X, my },
    };
    for (const auto& p : pts)
    {
        if (fabs(sx - p.x) <= hs && fabs(sy - p.y) <= hs)
            return p.mode;
    }
    return DRAG_NONE;
}

LPCWSTR CursorForMode(int mode)
{
    switch (mode)
    {
    case DRAG_TL: case DRAG_BR: return IDC_SIZENWSE;
    case DRAG_TR: case DRAG_BL: return IDC_SIZENESW;
    case DRAG_T:  case DRAG_B:  return IDC_SIZENS;
    case DRAG_L:  case DRAG_R:  return IDC_SIZEWE;
    case DRAG_MOVE: case DRAG_PAN: return IDC_SIZEALL;
    case DRAG_NEW: return IDC_CROSS;
    default: return IDC_ARROW;
    }
}

void UpdateDrag(EditorState* st, PointF p)
{
    double r = RatioValue(st);
    bool fixed = !st->freeRatio && r > 0.0;

    switch (st->drag)
    {
    case DRAG_MOVE:
    {
        float nx = st->cropStart.X + (p.X - st->dragStart.X);
        float ny = st->cropStart.Y + (p.Y - st->dragStart.Y);
        float maxX = (float)st->imgW - st->cropStart.Width;
        float maxY = (float)st->imgH - st->cropStart.Height;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx > maxX) nx = maxX;
        if (ny > maxY) ny = maxY;
        st->crop = RectF(nx, ny, st->cropStart.Width, st->cropStart.Height);
        break;
    }
    case DRAG_NEW:
    {
        float ax = st->dragStart.X, ay = st->dragStart.Y;
        float dx = p.X - ax, dy = p.Y - ay;
        float w = fabs(dx), h = fabs(dy);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        if (fixed)
        {
            if (w / h > r) w = h * (float)r;
            else h = w / (float)r;
        }
        float maxW = dx >= 0 ? (float)st->imgW - ax : ax;
        float maxH = dy >= 0 ? (float)st->imgH - ay : ay;
        if (maxW < 1) maxW = 1;
        if (maxH < 1) maxH = 1;
        if (w > maxW || h > maxH)
        {
            float s = std::min(maxW / w, maxH / h);
            w *= s;
            h *= s;
        }
        if (w < MIN_CROP || h < MIN_CROP)
            break;
        float x = dx >= 0 ? ax : ax - w;
        float y = dy >= 0 ? ay : ay - h;
        st->crop = RectF(x, y, w, h);
        break;
    }
    case DRAG_TL: case DRAG_T: case DRAG_TR: case DRAG_R:
    case DRAG_BR: case DRAG_B: case DRAG_BL: case DRAG_L:
    {
        RectF cs = st->cropStart;
        float anchorX = 0, anchorY = 0, w = 0, h = 0, x = 0, y = 0;
        bool corner = (st->drag == DRAG_TL || st->drag == DRAG_TR ||
                       st->drag == DRAG_BR || st->drag == DRAG_BL);

        if (corner)
        {
            switch (st->drag)
            {
            case DRAG_TL:
                anchorX = cs.X + cs.Width; anchorY = cs.Y + cs.Height;
                w = anchorX - p.X; h = anchorY - p.Y; break;
            case DRAG_TR:
                anchorX = cs.X; anchorY = cs.Y + cs.Height;
                w = p.X - anchorX; h = anchorY - p.Y; break;
            case DRAG_BR:
                anchorX = cs.X; anchorY = cs.Y;
                w = p.X - anchorX; h = p.Y - anchorY; break;
            case DRAG_BL:
                anchorX = cs.X + cs.Width; anchorY = cs.Y;
                w = anchorX - p.X; h = p.Y - anchorY; break;
            }
            if (w < 1) w = 1;
            if (h < 1) h = 1;
            if (fixed)
            {
                if (w / h > r) w = h * (float)r;
                else h = w / (float)r;
            }
            float spaceW = (st->drag == DRAG_TL || st->drag == DRAG_BL)
                               ? anchorX : (float)st->imgW - anchorX;
            float spaceH = (st->drag == DRAG_TL || st->drag == DRAG_TR)
                               ? anchorY : (float)st->imgH - anchorY;
            if (spaceW < 1) spaceW = 1;
            if (spaceH < 1) spaceH = 1;
            if (w > spaceW) w = spaceW;
            if (h > spaceH) h = spaceH;
            if (fixed)
            {
                if (w / h > r) w = h * (float)r;
                else h = w / (float)r;
            }
            if (w < MIN_CROP || h < MIN_CROP)
                break;
            x = (st->drag == DRAG_TL || st->drag == DRAG_BL) ? anchorX - w : anchorX;
            y = (st->drag == DRAG_TL || st->drag == DRAG_TR) ? anchorY - h : anchorY;
        }
        else if (st->drag == DRAG_T || st->drag == DRAG_B)
        {
            float cx = cs.X + cs.Width / 2.0f;
            if (st->drag == DRAG_T)
            {
                anchorY = cs.Y + cs.Height;
                h = anchorY - p.Y;
            }
            else
            {
                anchorY = cs.Y;
                h = p.Y - anchorY;
            }
            float spaceH = (st->drag == DRAG_T) ? anchorY : (float)st->imgH - anchorY;
            if (h < 1) h = 1;
            if (h > spaceH) h = spaceH;
            if (fixed)
            {
                w = h * (float)r;
                float maxW = 2.0f * std::min(cx, (float)st->imgW - cx);
                if (w > maxW)
                {
                    w = maxW;
                    h = w / (float)r;
                }
                if (w < MIN_CROP || h < MIN_CROP)
                    break;
                x = cx - w / 2.0f;
            }
            else
            {
                if (h < MIN_CROP)
                    break;
                w = cs.Width;
                x = cs.X;
            }
            y = (st->drag == DRAG_T) ? anchorY - h : anchorY;
        }
        else
        {
            float cy = cs.Y + cs.Height / 2.0f;
            if (st->drag == DRAG_L)
            {
                anchorX = cs.X + cs.Width;
                w = anchorX - p.X;
            }
            else
            {
                anchorX = cs.X;
                w = p.X - anchorX;
            }
            float spaceW = (st->drag == DRAG_L) ? anchorX : (float)st->imgW - anchorX;
            if (w < 1) w = 1;
            if (w > spaceW) w = spaceW;
            if (fixed)
            {
                h = w / (float)r;
                float maxH = 2.0f * std::min(cy, (float)st->imgH - cy);
                if (h > maxH)
                {
                    h = maxH;
                    w = h * (float)r;
                }
                if (w < MIN_CROP || h < MIN_CROP)
                    break;
                y = cy - h / 2.0f;
            }
            else
            {
                if (w < MIN_CROP)
                    break;
                h = cs.Height;
                y = cs.Y;
            }
            x = (st->drag == DRAG_L) ? anchorX - w : anchorX;
        }
        st->crop = RectF(x, y, w, h);
        break;
    }
    default:
        break;
    }
}

void FillRectSafe(Graphics& g, Brush* br, RectF r)
{
    if (r.Width <= 0 || r.Height <= 0)
        return;
    g.FillRectangle(br, r);
}

void DrawEditor(EditorState* st, Graphics& g, const RECT& rcClient)
{
    HDC hdc = g.GetHDC();
    FillRect(hdc, &rcClient, GetSysColorBrush(COLOR_BTNFACE));
    g.ReleaseHDC(hdc);

    SolidBrush bg(Color(255, 45, 45, 48));
    g.FillRectangle(&bg, RectF((REAL)st->canvasX, (REAL)st->canvasY,
                               (REAL)st->canvasW, (REAL)st->canvasH));
    if (!st->img)
        return;

    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetInterpolationMode(st->zoom >= 2.0 ? InterpolationModeNearestNeighbor
                                           : InterpolationModeHighQualityBicubic);

    PointF tl = ImgToScreen(st, 0, 0);
    float dw = (float)(st->imgW * st->zoom);
    float dh = (float)(st->imgH * st->zoom);
    g.DrawImage(st->img.get(), RectF(tl.X, tl.Y, dw, dh),
                0, 0, (REAL)st->imgW, (REAL)st->imgH, UnitPixel);

    PointF c0 = ImgToScreen(st, st->crop.X, st->crop.Y);
    PointF c1 = ImgToScreen(st, st->crop.X + st->crop.Width,
                            st->crop.Y + st->crop.Height);

    float cvx0 = (float)st->canvasX, cvy0 = (float)st->canvasY;
    float cvx1 = (float)(st->canvasX + st->canvasW);
    float cvy1 = (float)(st->canvasY + st->canvasH);
    float ix0 = std::max(tl.X, cvx0), iy0 = std::max(tl.Y, cvy0);
    float ix1 = std::min(tl.X + dw, cvx1), iy1 = std::min(tl.Y + dh, cvy1);
    float ccx0 = std::max(c0.X, cvx0), ccy0 = std::max(c0.Y, cvy0);
    float ccx1 = std::min(c1.X, cvx1), ccy1 = std::min(c1.Y, cvy1);

    SolidBrush dim(Color(150, 0, 0, 0));
    FillRectSafe(g, &dim, RectF(ix0, iy0, ix1 - ix0, ccy0 - iy0));
    FillRectSafe(g, &dim, RectF(ix0, ccy1, ix1 - ix0, iy1 - ccy1));
    FillRectSafe(g, &dim, RectF(ix0, ccy0, ccx0 - ix0, ccy1 - ccy0));
    FillRectSafe(g, &dim, RectF(ccx1, ccy0, ix1 - ccx1, ccy1 - ccy0));

    RectF cr(c0.X, c0.Y, c1.X - c0.X, c1.Y - c0.Y);
    if (cr.Width > 0 && cr.Height > 0)
    {
        Pen third(Color(90, 255, 255, 255), 1.0f);
        for (int i = 1; i <= 2; i++)
        {
            float x = cr.X + cr.Width * i / 3.0f;
            float y = cr.Y + cr.Height * i / 3.0f;
            g.DrawLine(&third, x, cr.Y, x, cr.Y + cr.Height);
            g.DrawLine(&third, cr.X, y, cr.X + cr.Width, y);
        }
        Pen outer(Color(255, 0, 0, 0), 3.0f);
        Pen inner(Color(255, 255, 255, 255), 1.0f);
        g.DrawRectangle(&outer, cr);
        g.DrawRectangle(&inner, cr);

        int hs = Sc(st, 4);
        SolidBrush hb(Color(255, 255, 255, 255));
        Pen hp(Color(255, 0, 0, 0), 1.0f);
        float xs[3] = { cr.X, cr.X + cr.Width / 2.0f, cr.X + cr.Width };
        float ys[3] = { cr.Y, cr.Y + cr.Height / 2.0f, cr.Y + cr.Height };
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 3; j++)
            {
                if (i == 1 && j == 1)
                    continue;
                RectF hr(xs[i] - hs, ys[j] - hs, hs * 2.0f, hs * 2.0f);
                g.FillRectangle(&hb, hr);
                g.DrawRectangle(&hp, hr);
            }
        }

        wchar_t buf[64];
        swprintf_s(buf, L"%d × %d px", (int)llround(st->crop.Width),
                   (int)llround(st->crop.Height));
        Font font(L"Yu Gothic UI", (REAL)Sc(st, 12), FontStyleRegular, UnitPixel);
        RectF textSize;
        g.MeasureString(buf, -1, &font, PointF(0, 0), &textSize);
        float lx = c0.X;
        float ly = c0.Y - textSize.Height - 6.0f;
        if (ly < cvy0 + 2.0f)
            ly = c0.Y + 4.0f;
        if (lx + textSize.Width + 10.0f > cvx1)
            lx = cvx1 - textSize.Width - 10.0f;
        if (lx < cvx0 + 2.0f)
            lx = cvx0 + 2.0f;
        SolidBrush lb(Color(170, 0, 0, 0));
        g.FillRectangle(&lb, RectF(lx, ly, textSize.Width + 10.0f, textSize.Height + 4.0f));
        SolidBrush tb(Color(255, 255, 255, 255));
        g.DrawString(buf, -1, &font, PointF(lx + 5.0f, ly + 2.0f), &tb);
    }

    wchar_t zbuf[64];
    swprintf_s(zbuf, L"ズーム %d%%", (int)llround(st->zoom * 100.0));
    Font zfont(L"Yu Gothic UI", (REAL)Sc(st, 12), FontStyleRegular, UnitPixel);
    SolidBrush zt(Color(200, 255, 255, 255));
    g.DrawString(zbuf, -1, &zfont, PointF(cvx0 + Sc(st, 8), cvy1 - Sc(st, 22)), &zt);
}

void EditorLayout(EditorState* st)
{
    RECT rc{};
    GetClientRect(st->hwnd, &rc);
    int pad = Sc(st, 8);
    int ch = Sc(st, 26);
    int toolH = ch + pad * 2;
    int panelW = Sc(st, 300);
    if (panelW > rc.right / 2)
        panelW = rc.right / 2;

    st->canvasX = 0;
    st->canvasY = toolH;
    st->canvasW = std::max(0, (int)rc.right - panelW);
    st->canvasH = std::max(0, (int)rc.bottom - toolH);

    int x = pad, y = pad;
    MoveWindow(st->cboRatio, x, y, Sc(st, 110), Sc(st, 240), TRUE);
    x += Sc(st, 110) + Sc(st, 6);
    MoveWindow(st->edRW, x, y, Sc(st, 46), ch, TRUE);
    x += Sc(st, 46) + Sc(st, 2);
    MoveWindow(st->lblColon, x, y, Sc(st, 12), ch, TRUE);
    x += Sc(st, 12) + Sc(st, 2);
    MoveWindow(st->edRH, x, y, Sc(st, 46), ch, TRUE);
    x += Sc(st, 46) + Sc(st, 12);
    MoveWindow(st->chkDpi, x, y, Sc(st, 130), ch, TRUE);
    x += Sc(st, 130) + Sc(st, 16);
    MoveWindow(st->btnRatioMax, x, y, Sc(st, 116), ch, TRUE);
    x += Sc(st, 116) + Sc(st, 6);
    MoveWindow(st->btnReset, x, y, Sc(st, 96), ch, TRUE);
    x += Sc(st, 96) + Sc(st, 16);
    MoveWindow(st->btnZoomFit, x, y, Sc(st, 64), ch, TRUE);
    x += Sc(st, 64) + Sc(st, 6);
    MoveWindow(st->btnZoom100, x, y, Sc(st, 64), ch, TRUE);

    int px = st->canvasW + pad;
    int pw = std::max(Sc(st, 120), panelW - pad * 2);
    int py = toolH + pad;

    int gh = Sc(st, 92);
    MoveWindow(st->grpCrop, px, py, pw, gh, TRUE);
    MoveWindow(st->stCrop, px + Sc(st, 10), py + Sc(st, 20), pw - Sc(st, 20), gh - Sc(st, 26), TRUE);
    py += gh + pad;

    gh = Sc(st, 120);
    MoveWindow(st->grpOut, px, py, pw, gh, TRUE);
    int cx = px + Sc(st, 10);
    int cy = py + Sc(st, 22);
    int ew = (pw - Sc(st, 70)) / 2;
    if (ew < Sc(st, 50)) ew = Sc(st, 50);
    MoveWindow(st->edOutW, cx, cy, ew, ch, TRUE);
    MoveWindow(st->lblOutX, cx + ew + Sc(st, 2), cy, Sc(st, 16), ch, TRUE);
    MoveWindow(st->edOutH, cx + ew + Sc(st, 18), cy, ew, ch, TRUE);
    MoveWindow(st->lblOutPx, cx + ew * 2 + Sc(st, 20), cy, Sc(st, 40), ch, TRUE);
    cy += ch + Sc(st, 10);
    MoveWindow(st->chkKeep, cx, cy, Sc(st, 130), ch, TRUE);
    MoveWindow(st->lblPct, cx + Sc(st, 140), cy, Sc(st, 36), ch, TRUE);
    MoveWindow(st->cboPct, cx + Sc(st, 180), cy, Sc(st, 96), Sc(st, 220), TRUE);
    py += gh + pad;

    gh = Sc(st, 92);
    MoveWindow(st->grpDpi, px, py, pw, gh, TRUE);
    cy = py + Sc(st, 22);
    MoveWindow(st->edDpiX, cx, cy, Sc(st, 70), ch, TRUE);
    MoveWindow(st->lblDpiX, cx + Sc(st, 72), cy, Sc(st, 16), ch, TRUE);
    MoveWindow(st->edDpiY, cx + Sc(st, 90), cy, Sc(st, 70), ch, TRUE);
    MoveWindow(st->lblDpiY, cx + Sc(st, 162), cy, Sc(st, 36), ch, TRUE);
    cy += ch + Sc(st, 8);
    MoveWindow(st->btnSrcDpi, cx, cy, Sc(st, 130), ch, TRUE);
    py += gh + pad;

    MoveWindow(st->stOutInfo, px, py, pw, Sc(st, 22), TRUE);
    py += Sc(st, 24);
    MoveWindow(st->stOutDir, px, py, pw, Sc(st, 20), TRUE);
    py += Sc(st, 22);
    MoveWindow(st->btnOutDir, px, py, pw, Sc(st, 30), TRUE);
    py += Sc(st, 36);
    MoveWindow(st->btnSave, px, py, pw, Sc(st, 40), TRUE);
    py += Sc(st, 46);
    MoveWindow(st->stStatus, px, py, pw, Sc(st, 22), TRUE);

    UpdateOutDirText(st);
}

void ApplyFont(EditorState* st)
{
    HWND controls[] = {
        st->cboRatio, st->edRW, st->edRH, st->chkDpi, st->btnRatioMax,
        st->btnReset, st->btnZoomFit, st->btnZoom100, st->lblColon,
        st->grpCrop, st->stCrop, st->grpOut, st->edOutW, st->edOutH,
        st->lblOutX, st->lblOutPx, st->chkKeep, st->lblPct, st->cboPct,
        st->grpDpi, st->edDpiX, st->edDpiY, st->btnSrcDpi, st->lblDpiX,
        st->lblDpiY, st->stOutInfo, st->stOutDir, st->btnOutDir,
        st->stStatus, st->btnSave,
    };
    for (HWND h : controls)
        if (h)
            SendMessageW(h, WM_SETFONT, (WPARAM)st->font, TRUE);
}

void RecreateFont(EditorState* st)
{
    if (st->font)
        DeleteObject(st->font);
    st->font = CreateFontW(-MulDiv(9, st->dpi, 72), 0, 0, 0, FW_NORMAL,
                           FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           L"Yu Gothic UI");
    ApplyFont(st);
}

HWND MakeControl(EditorState* st, LPCWSTR cls, LPCWSTR text, DWORD style, int id)
{
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             0, 0, 10, 10, st->hwnd, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    if (h && st->font)
        SendMessageW(h, WM_SETFONT, (WPARAM)st->font, TRUE);
    return h;
}

void CreateControls(EditorState* st)
{
    st->updating = true;

    st->cboRatio = MakeControl(st, WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_CBO_RATIO);
    st->edRW = MakeControl(st, WC_EDITW, L"1",
        ES_AUTOHSCROLL | ES_RIGHT | WS_TABSTOP, IDC_ED_RW);
    st->lblColon = MakeControl(st, WC_STATICW, L":",
        SS_CENTER | SS_CENTERIMAGE, IDC_LBL_COLON);
    st->edRH = MakeControl(st, WC_EDITW, L"1",
        ES_AUTOHSCROLL | ES_RIGHT | WS_TABSTOP, IDC_ED_RH);
    st->chkDpi = MakeControl(st, WC_BUTTONW, L"DPI基準(物理)",
        BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CHK_DPIBASIS);
    st->btnRatioMax = MakeControl(st, WC_BUTTONW, L"比率で最大",
        BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_RATIOMAX);
    st->btnReset = MakeControl(st, WC_BUTTONW, L"全体を選択",
        BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_RESET);
    st->btnZoomFit = MakeControl(st, WC_BUTTONW, L"Fit",
        BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_ZOOMFIT);
    st->btnZoom100 = MakeControl(st, WC_BUTTONW, L"100%",
        BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_ZOOM100);

    st->grpCrop = MakeControl(st, WC_BUTTONW, L"切り抜き",
        BS_GROUPBOX, IDC_GRP_CROP);
    st->stCrop = MakeControl(st, WC_STATICW, L"",
        SS_LEFT, IDC_ST_CROP);

    st->grpOut = MakeControl(st, WC_BUTTONW, L"出力サイズ (リサイズ)",
        BS_GROUPBOX, IDC_GRP_OUT);
    st->edOutW = MakeControl(st, WC_EDITW, L"1000",
        ES_AUTOHSCROLL | ES_RIGHT | WS_TABSTOP, IDC_ED_OUTW);
    st->lblOutX = MakeControl(st, WC_STATICW, L"×",
        SS_CENTER | SS_CENTERIMAGE, IDC_LBL_OUTX);
    st->edOutH = MakeControl(st, WC_EDITW, L"1000",
        ES_AUTOHSCROLL | ES_RIGHT | WS_TABSTOP, IDC_ED_OUTH);
    st->lblOutPx = MakeControl(st, WC_STATICW, L"px",
        SS_LEFT | SS_CENTERIMAGE, IDC_LBL_OUTPX);
    st->chkKeep = MakeControl(st, WC_BUTTONW, L"縦横比を維持",
        BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CHK_KEEP);
    st->lblPct = MakeControl(st, WC_STATICW, L"倍率",
        SS_LEFT | SS_CENTERIMAGE, IDC_LBL_PCT);
    st->cboPct = MakeControl(st, WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_CBO_PCT);

    st->grpDpi = MakeControl(st, WC_BUTTONW, L"出力解像度 (DPI)",
        BS_GROUPBOX, IDC_GRP_DPI);
    st->edDpiX = MakeControl(st, WC_EDITW, L"96",
        ES_AUTOHSCROLL | ES_RIGHT | WS_TABSTOP, IDC_ED_DPIX);
    st->lblDpiX = MakeControl(st, WC_STATICW, L"×",
        SS_CENTER | SS_CENTERIMAGE, IDC_LBL_DPIX);
    st->edDpiY = MakeControl(st, WC_EDITW, L"96",
        ES_AUTOHSCROLL | ES_RIGHT | WS_TABSTOP, IDC_ED_DPIY);
    st->lblDpiY = MakeControl(st, WC_STATICW, L"dpi",
        SS_LEFT | SS_CENTERIMAGE, IDC_LBL_DPIY);
    st->btnSrcDpi = MakeControl(st, WC_BUTTONW, L"元画像の値",
        BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_SRCDPI);

    st->stOutInfo = MakeControl(st, WC_STATICW, L"",
        SS_LEFT, IDC_ST_OUTINFO);
    st->stOutDir = MakeControl(st, WC_STATICW, L"",
        SS_LEFT | SS_CENTERIMAGE, IDC_ST_OUTDIR);
    st->btnOutDir = MakeControl(st, WC_BUTTONW, L"出力先フォルダを指定...",
        BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_OUTDIR);

    st->stStatus = MakeControl(st, WC_STATICW, L"",
        SS_LEFT, IDC_ST_STATUS);
    st->btnSave = MakeControl(st, WC_BUTTONW, L"名前を付けて保存...",
        BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_SAVE);

    for (int i = 0; i < PRESET_COUNT; i++)
        SendMessageW(st->cboRatio, CB_ADDSTRING, 0, (LPARAM)kPresets[i].label);
    SendMessageW(st->cboRatio, CB_SETCURSEL, PRESET_ONE_TO_ONE, 0);

    for (int i = 0; i < PCT_COUNT; i++)
    {
        wchar_t buf[16];
        swprintf_s(buf, L"%g%%", kPercents[i]);
        SendMessageW(st->cboPct, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    SendMessageW(st->cboPct, CB_ADDSTRING, 0, (LPARAM)L"カスタム");
    SendMessageW(st->cboPct, CB_SETCURSEL, PCT_DEFAULT, 0);

    SendMessageW(st->chkKeep, BM_SETCHECK, BST_CHECKED, 0);

    st->updating = false;
}

void SetCustomPct(EditorState* st)
{
    st->pctIndex = PCT_CUSTOM;
    st->updating = true;
    SendMessageW(st->cboPct, CB_SETCURSEL, PCT_CUSTOM, 0);
    st->updating = false;
}

void OnRatioChanged(EditorState* st)
{
    int sel = (int)SendMessageW(st->cboRatio, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= PRESET_COUNT)
        return;

    if (kPresets[sel].free)
    {
        st->freeRatio = true;
    }
    else
    {
        st->freeRatio = false;
        if (sel == PRESET_CUSTOM)
        {
            double w = 0, h = 0;
            if (!ParseNumber(GetText(st->edRW), &w) ||
                !ParseNumber(GetText(st->edRH), &h) || w <= 0 || h <= 0)
                return;
            st->ratioW = w;
            st->ratioH = h;
        }
        else
        {
            st->ratioW = kPresets[sel].w;
            st->ratioH = kPresets[sel].h;
            st->updating = true;
            SetText(st->edRW, NumToStr(st->ratioW));
            SetText(st->edRH, NumToStr(st->ratioH));
            st->updating = false;
        }
        ApplyRatioToCrop(st, false);
    }
    EnableRatioControls(st);
    UpdateInfoTexts(st);
    SyncOutSize(st);
    UpdateOutPhysicalText(st);
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

void OnRatioEditChanged(EditorState* st)
{
    double w = 0, h = 0;
    if (!ParseNumber(GetText(st->edRW), &w) ||
        !ParseNumber(GetText(st->edRH), &h) || w <= 0 || h <= 0)
        return;
    st->freeRatio = false;
    st->ratioW = w;
    st->ratioH = h;
    st->updating = true;
    SendMessageW(st->cboRatio, CB_SETCURSEL, PRESET_CUSTOM, 0);
    st->updating = false;
    EnableRatioControls(st);
    ApplyRatioToCrop(st, false);
    UpdateInfoTexts(st);
    SyncOutSize(st);
    UpdateOutPhysicalText(st);
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

void OnOutEditChanged(EditorState* st, int id)
{
    double v = 0;
    HWND h = (id == IDC_ED_OUTW) ? st->edOutW : st->edOutH;
    if (!ParseNumber(GetText(h), &v) || v < 1)
        return;

    if (SendMessageW(st->chkKeep, BM_GETCHECK, 0, 0) == BST_CHECKED)
    {
        double cw = st->crop.Width, ch = st->crop.Height;
        if (cw > 0 && ch > 0)
        {
            double other = (id == IDC_ED_OUTW) ? (v * ch / cw) : (v * cw / ch);
            if (other < 1) other = 1;
            st->updating = true;
            SetText(id == IDC_ED_OUTW ? st->edOutH : st->edOutW, NumToStr(floor(other + 0.5)));
            st->updating = false;
        }
    }
    SetCustomPct(st);
    UpdateOutPhysicalText(st);
}

void DoSave(EditorState* st)
{
    RoundCrop(st);

    double outWd = 0, outHd = 0;
    if (!ParseNumber(GetText(st->edOutW), &outWd) || outWd < 1 ||
        !ParseNumber(GetText(st->edOutH), &outHd) || outHd < 1)
    {
        MessageBoxW(st->hwnd, L"出力サイズを正しく入力してください。",
                    L"ImageEdit", MB_ICONWARNING);
        return;
    }
    int outW = (int)llround(outWd);
    int outH = (int)llround(outHd);
    double dpiX = 96, dpiY = 96;
    if (!ParseNumber(GetText(st->edDpiX), &dpiX) || dpiX <= 0 ||
        !ParseNumber(GetText(st->edDpiY), &dpiY) || dpiY <= 0)
    {
        MessageBoxW(st->hwnd, L"出力DPIを正しく入力してください。",
                    L"ImageEdit", MB_ICONWARNING);
        return;
    }

    std::filesystem::path p(st->path);
    std::wstring stem = p.stem().wstring();
    if (stem.empty())
        stem = L"image";
    std::wstring ext = p.extension().wstring();
    if (ext.empty() || !ImgUtil_IsSupportedExtension(ext))
        ext = L".png";

    std::wstring defaultName = stem + L"_edit" + ext;
    wchar_t fileBuf[2048];
    wcsncpy_s(fileBuf, defaultName.c_str(), _TRUNCATE);
    std::wstring filter =
        L"PNG (*.png)\0*.png\0JPEG (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0"
        L"BMP (*.bmp)\0*.bmp\0GIF (*.gif)\0*.gif\0TIFF (*.tif;*.tiff)\0*.tif;*.tiff\0"
        L"すべてのファイル\0*.*\0\0";

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = st->hwnd;
    ofn.lpstrFilter = filter.c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = (DWORD)(sizeof(fileBuf) / sizeof(fileBuf[0]));
    ofn.lpstrDefExt = ext.c_str() + 1;
    ofn.lpstrInitialDir = st->outputDir.empty() ? nullptr : st->outputDir.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    if (!GetSaveFileNameW(&ofn))
        return;

    RectF crop((REAL)st->crop.X, (REAL)st->crop.Y,
               (REAL)st->crop.Width, (REAL)st->crop.Height);
    std::wstring err;
    if (ImgUtil_SaveAs(fileBuf, st->img.get(), crop, outW, outH, dpiX, dpiY, &err))
    {
        SaveOutSizePrefs(st);
        std::wstring msg = L"保存しました: ";
        msg += std::filesystem::path(fileBuf).filename().wstring();
        SetText(st->stStatus, msg);
        if (st->owner)
            PostMessageW(st->owner, WM_APP_THUMBS_CHANGED, 0, 0);
    }
    else
    {
        MessageBoxW(st->hwnd, err.c_str(), L"保存エラー", MB_ICONERROR);
    }
}

LRESULT CALLBACK EditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    EditorState* st = GetState(hwnd);

    switch (msg)
    {
    case WM_NCCREATE:
    {
        auto cs = (CREATESTRUCTW*)lParam;
        st = (EditorState*)cs->lpCreateParams;
        st->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        return TRUE;
    }
    case WM_CREATE:
    {
        if (!st)
            return -1;
        st->dpi = GetDpiForWindowSafe(hwnd);
        st->font = CreateFontW(-MulDiv(9, st->dpi, 72), 0, 0, 0, FW_NORMAL,
                               FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                               L"Yu Gothic UI");
        CreateControls(st);

        st->img = ImgUtil_LoadBitmap(st->path);
        if (!st->img)
        {
            MessageBoxW(hwnd, L"画像を読み込めませんでした。", L"ImageEdit", MB_ICONERROR);
            return -1;
        }
        st->imgW = (int)st->img->GetWidth();
        st->imgH = (int)st->img->GetHeight();
        st->srcDpiX = st->img->GetHorizontalResolution();
        st->srcDpiY = st->img->GetVerticalResolution();
        if (st->srcDpiX <= 1 || st->srcDpiX > 100000) st->srcDpiX = 96.0;
        if (st->srcDpiY <= 1 || st->srcDpiY > 100000) st->srcDpiY = 96.0;

        std::wstring title = std::filesystem::path(st->path).filename().wstring();
        title += L" - ImageEdit";
        SetWindowTextW(hwnd, title.c_str());

        st->freeRatio = false;
        st->ratioW = 1;
        st->ratioH = 1;
        st->dpiBasis = false;
        st->crop = RectF(0, 0, (REAL)st->imgW, (REAL)st->imgH);
        ApplyRatioToCrop(st, true);
        EnableRatioControls(st);

        wchar_t b[64];
        swprintf_s(b, L"%g", st->srcDpiX);
        SetText(st->edDpiX, b);
        swprintf_s(b, L"%g", st->srcDpiY);
        SetText(st->edDpiY, b);

        st->outputDir = GetOutputDir();

        int prefW = 0, prefH = 0;
        if (GetOutSizePrefs(&prefW, &prefH))
        {
            wchar_t b2[32];
            st->updating = true;
            swprintf_s(b2, L"%d", prefW);
            SetText(st->edOutW, b2);
            swprintf_s(b2, L"%d", prefH);
            SetText(st->edOutH, b2);
            st->pctIndex = PCT_CUSTOM;
            SendMessageW(st->cboPct, CB_SETCURSEL, PCT_CUSTOM, 0);
            st->updating = false;
        }

        EditorLayout(st);
        UpdateInfoTexts(st);
        SyncOutSize(st);
        UpdateOutPhysicalText(st);
        return 0;
    }
    case WM_SIZE:
        if (st)
        {
            EditorLayout(st);
            if (!st->userZoom)
                ComputeFit(st);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_DPICHANGED:
        if (st)
        {
            st->dpi = HIWORD(wParam);
            RecreateFont(st);
            RECT* pr = (RECT*)lParam;
            SetWindowPos(hwnd, nullptr, pr->left, pr->top,
                         pr->right - pr->left, pr->bottom - pr->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            EditorLayout(st);
            if (!st->userZoom)
                ComputeFit(st);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_GETMINMAXINFO:
    {
        auto mmi = (MINMAXINFO*)lParam;
        int dpi = st ? st->dpi : (int)GetDpiForSystemSafe();
        mmi->ptMinTrackSize.x = MulDiv(760, dpi, 96);
        mmi->ptMinTrackSize.y = MulDiv(560, dpi, 96);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w > 0 && h > 0 && st)
        {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
            HGDIOBJ old = SelectObject(mem, bmp);
            {
                Graphics g(mem);
                DrawEditor(st, g, rc);
                g.Flush();
            }
            BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        if (st && LOWORD(lParam) == HTCLIENT)
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            int hit = DRAG_NONE;
            if (InCanvas(st, pt.x, pt.y))
            {
                hit = HitTestHandle(st, pt.x, pt.y);
                if (hit == DRAG_NONE)
                    hit = st->crop.Contains(ScreenToImg(st, pt.x, pt.y))
                              ? DRAG_MOVE : DRAG_NEW;
            }
            SetCursor(LoadCursorW(nullptr, CursorForMode(hit)));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
    {
        if (!st || !InCanvas(st, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)))
            return 0;
        SetFocus(hwnd);
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        PointF ip = ScreenToImg(st, x, y);
        int hit = HitTestHandle(st, x, y);
        if (hit == DRAG_NONE)
            hit = st->crop.Contains(ip) ? DRAG_MOVE : DRAG_NEW;
        st->drag = hit;
        st->dragStart = ip;
        st->cropStart = st->crop;
        if (hit == DRAG_NEW)
            st->crop = RectF(ip.X, ip.Y, 1.0f, 1.0f);
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (!st)
            return 0;
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        if (st->drag == DRAG_PAN)
        {
            st->origin.X = st->panStartOrigin.X + (float)(x - st->panStartScreen.x);
            st->origin.Y = st->panStartOrigin.Y + (float)(y - st->panStartScreen.y);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (st->drag != DRAG_NONE)
        {
            PointF ip = ScreenToImg(st, x, y);
            UpdateDrag(st, ip);
            UpdateInfoTexts(st);
            SyncOutSize(st);
            UpdateOutPhysicalText(st);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (InCanvas(st, x, y))
        {
            int hit = HitTestHandle(st, x, y);
            if (hit == DRAG_NONE)
                hit = st->crop.Contains(ScreenToImg(st, x, y))
                          ? DRAG_MOVE : DRAG_NEW;
            SetCursor(LoadCursorW(nullptr, CursorForMode(hit)));
        }
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (!st)
            return 0;
        if (st->drag != DRAG_NONE && st->drag != DRAG_PAN)
        {
            RoundCrop(st);
            st->drag = DRAG_NONE;
            ReleaseCapture();
            UpdateInfoTexts(st);
            SyncOutSize(st);
            UpdateOutPhysicalText(st);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MBUTTONDOWN:
        if (st && InCanvas(st, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)))
        {
            st->drag = DRAG_PAN;
            st->panStartOrigin = st->origin;
            st->panStartScreen.x = GET_X_LPARAM(lParam);
            st->panStartScreen.y = GET_Y_LPARAM(lParam);
            SetCapture(hwnd);
        }
        return 0;
    case WM_MBUTTONUP:
        if (st && st->drag == DRAG_PAN)
        {
            st->drag = DRAG_NONE;
            ReleaseCapture();
        }
        return 0;
    case WM_MOUSEWHEEL:
    {
        if (!st)
            return 0;
        POINT pt;
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        ScreenToClient(hwnd, &pt);
        if (!InCanvas(st, pt.x, pt.y))
            return 0;
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        double factor = delta > 0 ? 1.25 : 0.8;
        double nz = st->zoom * factor;
        nz = std::max(0.02, std::min(32.0, nz));
        PointF ip = ScreenToImg(st, pt.x, pt.y);
        st->zoom = nz;
        st->origin.X = (float)(pt.x - ip.X * nz);
        st->origin.Y = (float)(pt.y - ip.Y * nz);
        st->userZoom = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_COMMAND:
    {
        if (!st)
            return 0;
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        switch (id)
        {
        case IDC_CBO_RATIO:
            if (code == CBN_SELCHANGE && !st->updating)
                OnRatioChanged(st);
            return 0;
        case IDC_ED_RW:
        case IDC_ED_RH:
            if (code == EN_CHANGE && !st->updating)
                OnRatioEditChanged(st);
            return 0;
        case IDC_CHK_DPIBASIS:
            if (code == BN_CLICKED && !st->updating)
            {
                st->dpiBasis =
                    SendMessageW(st->chkDpi, BM_GETCHECK, 0, 0) == BST_CHECKED;
                ApplyRatioToCrop(st, false);
                UpdateInfoTexts(st);
                SyncOutSize(st);
                UpdateOutPhysicalText(st);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case IDC_BTN_RATIOMAX:
            if (code == BN_CLICKED)
            {
                ApplyRatioToCrop(st, true);
                UpdateInfoTexts(st);
                SyncOutSize(st);
                UpdateOutPhysicalText(st);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case IDC_BTN_RESET:
            if (code == BN_CLICKED)
            {
                st->freeRatio = true;
                st->updating = true;
                SendMessageW(st->cboRatio, CB_SETCURSEL, PRESET_FREE, 0);
                st->updating = false;
                EnableRatioControls(st);
                st->crop = RectF(0, 0, (REAL)st->imgW, (REAL)st->imgH);
                UpdateInfoTexts(st);
                SyncOutSize(st);
                UpdateOutPhysicalText(st);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case IDC_BTN_ZOOMFIT:
            if (code == BN_CLICKED)
            {
                st->userZoom = false;
                ComputeFit(st);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case IDC_BTN_ZOOM100:
            if (code == BN_CLICKED)
            {
                st->zoom = 1.0;
                st->userZoom = true;
                st->origin.X = (float)(st->canvasX +
                    (st->canvasW - st->imgW) / 2.0);
                st->origin.Y = (float)(st->canvasY +
                    (st->canvasH - st->imgH) / 2.0);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case IDC_CBO_PCT:
            if (code == CBN_SELCHANGE && !st->updating)
            {
                int sel = (int)SendMessageW(st->cboPct, CB_GETCURSEL, 0, 0);
                if (sel >= 0)
                {
                    st->pctIndex = sel;
                    if (sel < PCT_CUSTOM)
                    {
                        SyncOutSize(st);
                        UpdateOutPhysicalText(st);
                    }
                }
            }
            return 0;
        case IDC_ED_OUTW:
        case IDC_ED_OUTH:
            if (code == EN_CHANGE && !st->updating)
                OnOutEditChanged(st, id);
            else if (code == EN_KILLFOCUS)
                SaveOutSizePrefs(st);
            return 0;
        case IDC_ED_DPIX:
        case IDC_ED_DPIY:
            if (code == EN_CHANGE && !st->updating)
                UpdateOutPhysicalText(st);
            return 0;
        case IDC_BTN_SRCDPI:
            if (code == BN_CLICKED)
            {
                wchar_t b[64];
                swprintf_s(b, L"%g", st->srcDpiX);
                SetText(st->edDpiX, b);
                swprintf_s(b, L"%g", st->srcDpiY);
                SetText(st->edDpiY, b);
                UpdateOutPhysicalText(st);
            }
            return 0;
        case IDC_BTN_OUTDIR:
            if (code == BN_CLICKED)
            {
                std::wstring dir = PickFolderDialog(hwnd, L"出力先フォルダを指定",
                                                   st->outputDir);
                if (!dir.empty())
                {
                    st->outputDir = dir;
                    SetOutputDir(dir);
                    UpdateOutDirText(st);
                }
            }
            return 0;
        case IDC_BTN_SAVE:
            if (code == BN_CLICKED)
                DoSave(st);
            return 0;
        }
        break;
    }
    case WM_NCDESTROY:
        if (st)
        {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (st->font)
                DeleteObject(st->font);
            delete st;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

HWND Editor_Open(HWND owner, const std::wstring& path)
{
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = EditorWndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ImageEditEditorWnd";
        if (!RegisterClassExW(&wc))
            return nullptr;
        registered = true;
    }

    auto st = new EditorState();
    st->owner = owner;
    st->path = path;

    UINT dpi = GetDpiForSystemSafe();
    HWND h = CreateWindowExW(
        0, L"ImageEditEditorWnd", L"ImageEdit",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1180, dpi, 96), MulDiv(800, dpi, 96),
        owner, nullptr, inst, st);
    if (!h)
        return nullptr;
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    return h;
}
