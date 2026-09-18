#pragma once

#include "domain/models.h"

std::wstring win32DataPath();
bool loadAppData(AppData& data, std::wstring& sourcePath);
bool saveAppData(const AppData& data);
bool loadAppDataFromPath(const std::wstring& path, AppData& data);
bool saveAppDataToPath(const AppData& data, const std::wstring& path);
