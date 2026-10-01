#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <memory>
#include <string>

bool ImgUtil_IsSupportedFile(const std::wstring& path);
bool ImgUtil_IsSupportedExtension(const std::wstring& ext);

std::unique_ptr<Gdiplus::Bitmap> ImgUtil_LoadBitmap(const std::wstring& path);

bool ImgUtil_LoadInfo(const std::wstring& path, int* width, int* height,
                      double* dpiX, double* dpiY);

HBITMAP ImgUtil_CreateThumbnail(const std::wstring& path, int boxW, int boxH,
                                int* srcW, int* srcH);

bool ImgUtil_SaveAs(const std::wstring& dstPath, Gdiplus::Bitmap* src,
                    const Gdiplus::RectF& crop, int outW, int outH,
                    double dpiX, double dpiY, std::wstring* err);
