#include "webp_codec.h"

#include <webp/decode.h>
#include <webp/encode.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace Gdiplus;

namespace {

bool ReadAllBytes(const std::wstring& path, std::vector<uint8_t>* data)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER li{};
    if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0 || li.QuadPart > 0x7FFFFFFF)
    {
        CloseHandle(h);
        return false;
    }

    data->resize((size_t)li.QuadPart);
    DWORD total = 0;
    BOOL ok = ReadFile(h, data->data(), (DWORD)data->size(), &total, nullptr);
    CloseHandle(h);
    return ok && total == data->size();
}

bool WriteAllBytes(const std::wstring& path, const uint8_t* data, size_t size)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    BOOL ok = WriteFile(h, data, (DWORD)size, &written, nullptr);
    CloseHandle(h);
    return ok && written == size;
}

} // namespace

std::unique_ptr<Bitmap> WebPCodec_Load(const std::wstring& path)
{
    std::vector<uint8_t> data;
    if (!ReadAllBytes(path, &data))
        return nullptr;

    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(data.data(), data.size(), &features) != VP8_STATUS_OK)
        return nullptr;

    int w = features.width;
    int h = features.height;
    if (w <= 0 || h <= 0)
        return nullptr;

    int stride = w * 4;
    std::vector<uint8_t> bgra((size_t)stride * h);
    if (!WebPDecodeBGRAInto(data.data(), data.size(), bgra.data(), bgra.size(), stride))
        return nullptr;

    Bitmap raw(w, h, stride, PixelFormat32bppARGB, bgra.data());
    if (raw.GetLastStatus() != Ok)
        return nullptr;

    std::unique_ptr<Bitmap> copy(raw.Clone(0, 0, w, h, PixelFormat32bppARGB));
    if (!copy || copy->GetLastStatus() != Ok)
        return nullptr;

    copy->SetResolution(96.0f, 96.0f);
    return copy;
}

bool WebPCodec_LoadInfo(const std::wstring& path, int* width, int* height)
{
    std::vector<uint8_t> data;
    if (!ReadAllBytes(path, &data))
        return false;

    int w = 0;
    int h = 0;
    if (!WebPGetInfo(data.data(), data.size(), &w, &h))
        return false;
    if (w <= 0 || h <= 0)
        return false;

    if (width) *width = w;
    if (height) *height = h;
    return true;
}

bool WebPCodec_Save(const std::wstring& path, Bitmap* src, float quality,
                    std::wstring* err)
{
    if (!src || src->GetLastStatus() != Ok)
    {
        if (err) *err = L"画像が読み込まれていません。";
        return false;
    }

    INT w = (INT)src->GetWidth();
    INT h = (INT)src->GetHeight();
    if (w <= 0 || h <= 0)
    {
        if (err) *err = L"出力サイズが不正です。";
        return false;
    }

    int stride = w * 4;
    std::vector<uint8_t> bgra((size_t)stride * h);

    Rect rect(0, 0, w, h);
    BitmapData bd{};
    if (src->LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok)
    {
        if (err) *err = L"画像データを読み出せませんでした。";
        return false;
    }

    for (INT y = 0; y < h; y++)
    {
        const uint8_t* row = (const uint8_t*)bd.Scan0 + (ptrdiff_t)y * bd.Stride;
        memcpy(bgra.data() + (size_t)y * stride, row, (size_t)stride);
    }
    src->UnlockBits(&bd);

    uint8_t* out = nullptr;
    size_t outSize = WebPEncodeBGRA(bgra.data(), w, h, stride, quality, &out);
    if (outSize == 0 || !out)
    {
        if (err) *err = L"WebP エンコードに失敗しました。";
        return false;
    }

    bool ok = WriteAllBytes(path, out, outSize);
    WebPFree(out);

    if (!ok)
    {
        if (err) *err = L"WebP ファイルを書き込めませんでした。";
        return false;
    }
    return true;
}
