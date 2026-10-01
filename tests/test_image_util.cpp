#include "image_util.h"

#include <windows.h>
#include <gdiplus.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

using namespace Gdiplus;

static int g_fail = 0;

static void Check(bool ok, const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    wchar_t msg[1024];
    vswprintf_s(msg, fmt, ap);
    va_end(ap);
    if (ok)
    {
        wprintf(L"PASS: %s\n", msg);
    }
    else
    {
        wprintf(L"FAIL: %s\n", msg);
        g_fail++;
    }
}

int wmain()
{
    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gsi, nullptr) != Ok)
    {
        wprintf(L"FAIL: GdiplusStartup\n");
        return 1;
    }

    wchar_t tmp[MAX_PATH] = L"";
    if (!GetTempPathW(MAX_PATH, tmp))
        wcscpy_s(tmp, L".\\");
    std::wstring dir = tmp;
    std::wstring srcPath = dir + L"imgedit_test_src.png";
    std::wstring outPath = dir + L"imgedit_test_out.png";

    {
        Bitmap bmp(400, 300, PixelFormat32bppARGB);
        Graphics g(&bmp);
        g.Clear(Color(255, 255, 255, 255));
        SolidBrush red(Color(255, 255, 0, 0));
        SolidBrush blue(Color(255, 0, 0, 255));
        g.FillRectangle(&red, 0, 0, 200, 300);
        g.FillRectangle(&blue, 200, 0, 200, 300);
        g.Flush();
        bmp.SetResolution(300.0f, 600.0f);

        std::wstring err;
        bool ok = ImgUtil_SaveAs(srcPath, &bmp, RectF(0, 0, 400, 300),
                                 400, 300, 300, 600, &err);
        Check(ok, L"create source png (err=%s)", err.c_str());
    }

    int w = 0, h = 0;
    double dx = 0, dy = 0;
    Check(ImgUtil_LoadInfo(srcPath, &w, &h, &dx, &dy), L"LoadInfo source");
    Check(w == 400 && h == 300, L"source size 400x300 (got %dx%d)", w, h);
    Check(fabs(dx - 300) < 1.0 && fabs(dy - 600) < 1.0,
          L"source dpi 300x600 persisted (got %g x %g)", dx, dy);

    {
        std::unique_ptr<Bitmap> bmp = ImgUtil_LoadBitmap(srcPath);
        Check(bmp != nullptr, L"LoadBitmap");
        if (bmp)
        {
            Check((int)bmp->GetWidth() == 400 && (int)bmp->GetHeight() == 300,
                  L"loaded size 400x300");
            Check(fabs(bmp->GetHorizontalResolution() - 300) < 1.0 &&
                      fabs(bmp->GetVerticalResolution() - 600) < 1.0,
                  L"loaded dpi 300x600");

            std::wstring err;
            bool ok = ImgUtil_SaveAs(outPath, bmp.get(), RectF(100, 100, 200, 100),
                                     100, 50, 96, 96, &err);
            Check(ok, L"crop+resize save (err=%s)", err.c_str());
        }
    }

    w = h = 0;
    dx = dy = 0;
    Check(ImgUtil_LoadInfo(outPath, &w, &h, &dx, &dy), L"LoadInfo output");
    Check(w == 100 && h == 50, L"output size 100x50 (got %dx%d)", w, h);
    Check(fabs(dx - 96) < 1.0 && fabs(dy - 96) < 1.0,
          L"output dpi 96 (got %g x %g)", dx, dy);

    {
        int sw = 0, sh = 0;
        HBITMAP hbm = ImgUtil_CreateThumbnail(srcPath, 80, 80, &sw, &sh);
        Check(hbm != nullptr, L"create thumbnail");
        if (hbm)
        {
            BITMAP bm{};
            GetObjectW(hbm, sizeof(bm), &bm);
            Check(bm.bmWidth == 80 && bm.bmHeight == 60,
                  L"thumbnail size 80x60 (got %dx%d)", bm.bmWidth, bm.bmHeight);
            DWORD* px = (DWORD*)bm.bmBits;
            Check(px != nullptr && (px[0] >> 24) == 255,
                  L"thumbnail alpha opaque");
            DeleteObject(hbm);
        }
    }

    {
        wchar_t jpgPath[MAX_PATH];
        wcscpy_s(jpgPath, outPath.c_str());
        std::wstring s = jpgPath;
        s = s.substr(0, s.size() - 3) + L"jpg";
        std::unique_ptr<Bitmap> bmp = ImgUtil_LoadBitmap(srcPath);
        std::wstring err;
        bool ok = bmp && ImgUtil_SaveAs(s, bmp.get(), RectF(0, 0, 400, 300),
                                        200, 150, 72, 72, &err);
        Check(ok, L"save jpeg (err=%s)", err.c_str());
        int jw = 0, jh = 0;
        double jdx = 0, jdy = 0;
        Check(ImgUtil_LoadInfo(s, &jw, &jh, &jdx, &jdy), L"LoadInfo jpeg");
        Check(jw == 200 && jh == 150, L"jpeg size 200x150 (got %dx%d)", jw, jh);
        DeleteFileW(s.c_str());
    }

    DeleteFileW(srcPath.c_str());
    DeleteFileW(outPath.c_str());

    GdiplusShutdown(token);

    if (g_fail)
    {
        wprintf(L"\n%d test(s) FAILED\n", g_fail);
        return 1;
    }
    wprintf(L"\nAll tests passed\n");
    return 0;
}
