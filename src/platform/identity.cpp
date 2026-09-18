#include "platform/identity.h"
#include "platform/windows.h"
#include <objbase.h>
#include <cstdio>

using std::string;

string newId() {
    GUID guid{};
    CoCreateGuid(&guid);
    char text[33]{};
    snprintf(text, sizeof(text), "%08x%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
        guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return text;
}
