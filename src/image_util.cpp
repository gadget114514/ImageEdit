#include "image_util.h"

#include <shlwapi.h>
#include <algorithm>
#include <cwctype>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace Gdiplus;

namespace {

std::wstring ToLower(std::wstring s)
{
    for (auto& c : s)
        c = (wchar_t)towlower(c);
    return s;
}

std::wstring ExtensionOf(const std::wstring& path)
{
    size_t dot = path.find_last_of(L'.');
    size_t sep = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos)
        return L"";
    if (sep != std::wstring::npos && dot < sep)
        return L"";
    return ToLower(path.substr(dot + 1));
}

const wchar_t* MimeFromExt(const std::wstring& ext)
{
    if (ext == L"png") return L"image/png";
    if (ext == L"jpg" || ext == L"jpeg") return L"image/jpeg";
    if (ext == L"bmp") return L"image/bmp";
    if (ext == L"gif") return L"image/gif";
    if (ext == L"tif" || ext == L"tiff") return L"image/tiff";
    return nullptr;
}

bool HasAlphaFormat(const std::wstring& ext)
{
    return ext == L"png" || ext == L"gif" || ext == L"tif" || ext == L"tiff";
}

int GetEncoderClsid(const wchar_t* mime, CLSID* clsid)
{
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0)
        return -1;
    std::vector<BYTE> buf(size);
    ImageCodecInfo* infos = (ImageCodecInfo*)buf.data();
    if (GetImageEncoders(num, size, infos) != Ok)
        return -1;
    for (UINT i = 0; i < num; i++)
    {
        if (wcscmp(infos[i].MimeType, mime) == 0)
        {
            *clsid = infos[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

double NormalizeDpi(REAL dpi)
{
    if (dpi <= 1.0f || dpi > 100000.0f)
        return 96.0;
    return (double)dpi;
}

} // namespace

bool ImgUtil_IsSupportedExtension(const std::wstring& ext)
{
    return MimeFromExt(ToLower(ext)) != nullptr;
}

bool ImgUtil_IsSupportedFile(const std::wstring& path)
{
    return ImgUtil_IsSupportedExtension(ExtensionOf(path));
}

std::unique_ptr<Bitmap> ImgUtil_LoadBitmap(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return nullptr;

    LARGE_INTEGER li{};
    if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0 || li.QuadPart > 0x7FFFFFFF)
    {
        CloseHandle(h);
        return nullptr;
    }

    std::vector<BYTE> data((size_t)li.QuadPart);
    DWORD total = 0;
    BOOL ok = ReadFile(h, data.data(), (DWORD)data.size(), &total, nullptr);
    CloseHandle(h);
    if (!ok || total != data.size())
        return nullptr;

    IStream* stream = SHCreateMemStream(data.data(), (UINT)data.size());
    if (!stream)
        return nullptr;

    Bitmap* raw = Bitmap::FromStream(stream);
    stream->Release();
    if (!raw || raw->GetLastStatus() != Ok)
    {
        delete raw;
        return nullptr;
    }

    REAL rx = raw->GetHorizontalResolution();
    REAL ry = raw->GetVerticalResolution();
    std::unique_ptr<Bitmap> copy(
        raw->Clone(0, 0, (INT)raw->GetWidth(), (INT)raw->GetHeight(), PixelFormat32bppARGB));
    delete raw;

    if (!copy || copy->GetLastStatus() != Ok)
        return nullptr;

    copy->SetResolution((REAL)NormalizeDpi(rx), (REAL)NormalizeDpi(ry));
    return copy;
}

bool ImgUtil_LoadInfo(const std::wstring& path, int* width, int* height,
                      double* dpiX, double* dpiY)
{
    Bitmap bmp(path.c_str(), FALSE);
    if (bmp.GetLastStatus() != Ok)
        return false;

    INT w = (INT)bmp.GetWidth();
    INT h = (INT)bmp.GetHeight();
    if (w <= 0 || h <= 0)
        return false;

    if (width) *width = w;
    if (height) *height = h;
    if (dpiX) *dpiX = NormalizeDpi(bmp.GetHorizontalResolution());
    if (dpiY) *dpiY = NormalizeDpi(bmp.GetVerticalResolution());
    return true;
}

HBITMAP ImgUtil_CreateThumbnail(const std::wstring& path, int boxW, int boxH,
                                int* srcW, int* srcH)
{
    std::unique_ptr<Bitmap> bmp = ImgUtil_LoadBitmap(path);
    if (!bmp)
        return nullptr;

    int sw = (int)bmp->GetWidth();
    int sh = (int)bmp->GetHeight();
    if (sw <= 0 || sh <= 0)
        return nullptr;
    if (srcW) *srcW = sw;
    if (srcH) *srcH = sh;

    double scale = std::min((double)boxW / sw, (double)boxH / sh);
    if (scale > 1.0)
        scale = 1.0;
    int tw = std::max(1, (int)(sw * scale));
    int th = std::max(1, (int)(sh * scale));

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = tw;
    bmi.bmiHeader.biHeight = -th;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm)
        return nullptr;

    HDC hdc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(hdc, hbm);
    {
        Graphics g(hdc);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.Clear(Color(255, 255, 255, 255));
        g.DrawImage(bmp.get(), Rect(0, 0, tw, th), 0, 0, sw, sh, UnitPixel);
        g.Flush();
    }
    SelectObject(hdc, old);
    DeleteDC(hdc);

    if (bits)
    {
        DWORD* px = (DWORD*)bits;
        int n = tw * th;
        for (int i = 0; i < n; i++)
            px[i] |= 0xFF000000u;
    }
    return hbm;
}

bool ImgUtil_SaveAs(const std::wstring& dstPath, Bitmap* src, const RectF& crop,
                    int outW, int outH, double dpiX, double dpiY, std::wstring* err)
{
    if (!src || src->GetLastStatus() != Ok)
    {
        if (err) *err = L"画像が読み込まれていません。";
        return false;
    }
    if (outW < 1 || outH < 1)
    {
        if (err) *err = L"出力サイズが不正です。";
        return false;
    }

    std::wstring ext = ExtensionOf(dstPath);
    const wchar_t* mime = MimeFromExt(ext);
    if (!mime)
    {
        if (err) *err = L"対応していない保存形式です (.png/.jpg/.bmp/.gif/.tif)。";
        return false;
    }

    CLSID clsid;
    if (GetEncoderClsid(mime, &clsid) < 0)
    {
        if (err) *err = L"画像エンコーダが見つかりません。";
        return false;
    }

    bool alpha = HasAlphaFormat(ext);
    RectF fixedCrop(crop.X, crop.Y, crop.Width, crop.Height);
    if (fixedCrop.Width < 1.0f) fixedCrop.Width = 1.0f;
    if (fixedCrop.Height < 1.0f) fixedCrop.Height = 1.0f;
    if (fixedCrop.X < 0) fixedCrop.X = 0;
    if (fixedCrop.Y < 0) fixedCrop.Y = 0;

    Bitmap dst(outW, outH, alpha ? PixelFormat32bppARGB : PixelFormat24bppRGB);
    if (dst.GetLastStatus() != Ok)
    {
        if (err) *err = L"出力ビットマップを作成できませんでした。";
        return false;
    }

    {
        Graphics g(&dst);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        if (!alpha)
            g.Clear(Color(255, 255, 255, 255));
        g.DrawImage(src, RectF(0, 0, (REAL)outW, (REAL)outH),
                    fixedCrop.X, fixedCrop.Y, fixedCrop.Width, fixedCrop.Height, UnitPixel);
        g.Flush();
        if (g.GetLastStatus() != Ok)
        {
            if (err) *err = L"画像の変換に失敗しました。";
            return false;
        }
    }

    if (dpiX > 0 && dpiY > 0)
    {
        if (dpiX > 100000) dpiX = 100000;
        if (dpiY > 100000) dpiY = 100000;
        dst.SetResolution((REAL)dpiX, (REAL)dpiY);
    }

    EncoderParameters encParams{};
    if (wcscmp(mime, L"image/jpeg") == 0)
    {
        encParams.Count = 1;
        encParams.Parameter[0].Guid = EncoderQuality;
        encParams.Parameter[0].Type = EncoderParameterValueTypeLong;
        encParams.Parameter[0].NumberOfValues = 1;
        ULONG quality = 92;
        encParams.Parameter[0].Value = &quality;
    }

    Status st = dst.Save(dstPath.c_str(), &clsid, encParams.Count ? &encParams : nullptr);
    if (st != Ok)
    {
        if (err)
        {
            wchar_t buf[128];
            swprintf_s(buf, L"保存に失敗しました (GDI+ status=%d)。", (int)st);
            *err = buf;
        }
        return false;
    }
    return true;
}
