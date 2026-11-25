#pragma once
#include "platform/windows.h"
struct EnvironmentSettings;
using SaveEnvironments=bool (*)(const EnvironmentSettings&);
void showEnvironmentPage(HINSTANCE instance, HWND owner, EnvironmentSettings& data, bool adding, SaveEnvironments persist);
