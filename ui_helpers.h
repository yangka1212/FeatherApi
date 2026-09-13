#pragma once

#include "app.h"
#include <commctrl.h>

inline std::wstring listCellText(HWND list, int row, int column) {
    std::wstring text(256, L'\0');
    for (;;) {
        LVITEMW item{};
        item.iSubItem = column;
        item.pszText = text.data();
        item.cchTextMax = (int)text.size();
        int count = (int)SendMessageW(list, LVM_GETITEMTEXTW, (WPARAM)row, (LPARAM)&item);
        if (count < (int)text.size() - 1) {
            text.resize((size_t)count);
            return text;
        }
        text.resize(text.size() * 2);
    }
}
