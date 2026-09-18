#include "ui/main_window.h"
#include "infrastructure/storage.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const MainWindowServices services{loadAppData, saveAppData, TaskDialogIndirect};
    return runMainWindow(instance, showCommand, services);
}
