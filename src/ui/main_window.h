#pragma once

#include "domain/models.h"
#include "platform/windows.h"
#include <commctrl.h>

// Dependencies are supplied by the executable composition root (or a test fixture).
struct MainWindowServices {
    bool (*loadData)(AppData&, std::wstring&);
    bool (*saveData)(const AppData&);
    HRESULT (WINAPI *showTaskDialog)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
};

int runMainWindow(HINSTANCE instance, int showCommand, const MainWindowServices& services);
