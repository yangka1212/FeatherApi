#include "ui/main_window.h"
#include "infrastructure/storage.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const MainWindowServices services{loadAppData, saveAppData,
        [](EnvironmentSettings& env){return loadEnvironmentSettings(env,win32DataPath());},
        [](const EnvironmentSettings& env){return saveEnvironmentSettings(env,win32DataPath());}, TaskDialogIndirect};
    return runMainWindow(instance, showCommand, services);
}
