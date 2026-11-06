#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <memory>
#include <string>

std::unique_ptr<Gdiplus::Bitmap> WebPCodec_Load(const std::wstring& path);
bool WebPCodec_LoadInfo(const std::wstring& path, int* width, int* height);

bool WebPCodec_Save(const std::wstring& path, Gdiplus::Bitmap* src, float quality,
                    std::wstring* err);
