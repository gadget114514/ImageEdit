#pragma once

#include <windows.h>
#include <string>
#include <vector>

HRESULT ImageEdit_DoDragDrop(HWND owner, const std::vector<std::wstring>& paths);
