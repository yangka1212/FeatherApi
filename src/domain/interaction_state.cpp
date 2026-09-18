#include "domain/interaction_state.h"
#include <algorithm>
#include <cwchar>

int tabSelectionAfterClose(int selectedIndex,int closedIndex,int tabCountBeforeClose) {
    if(tabCountBeforeClose<=1)return -1;
    if(closedIndex==selectedIndex)return std::min(closedIndex,tabCountBeforeClose-2);
    return closedIndex<selectedIndex?selectedIndex-1:selectedIndex;
}

bool listCheckboxStateChanged(unsigned oldState,unsigned newState) {
    constexpr unsigned stateImageMask=0xF000;
    const unsigned oldCheck=oldState&stateImageMask,newCheck=newState&stateImageMask;
    return oldCheck!=newCheck&&newCheck!=0;
}

std::wstring formatBytes(size_t bytes) {
    if(bytes<1024)return std::to_wstring(bytes)+L" B";
    double value=bytes<1024*1024?bytes/1024.0:bytes/1024.0/1024.0;
    wchar_t text[64]{};std::swprintf(text,64,L"%.2f",value);std::wstring number=text;
    while(!number.empty()&&number.back()==L'0')number.pop_back();if(!number.empty()&&number.back()==L'.')number.pop_back();
    return number+(bytes<1024*1024?L" KB":L" MB");
}
