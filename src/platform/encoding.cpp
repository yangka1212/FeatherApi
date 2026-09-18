#include "platform/encoding.h"
#include "platform/windows.h"

using std::string;
using std::wstring;

wstring toWide(const string& value) {
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(), nullptr, 0);
    if (!count) count = MultiByteToWideChar(CP_ACP, 0, value.data(), (int)value.size(), nullptr, 0);
    wstring result((size_t)count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(), result.data(), count))
        MultiByteToWideChar(CP_ACP, 0, value.data(), (int)value.size(), result.data(), count);
    return result;
}

string toUtf8(const wstring& value) {
    if (value.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0, nullptr, nullptr);
    string result((size_t)count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), result.data(), count, nullptr, nullptr);
    return result;
}
