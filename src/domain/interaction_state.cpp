#include "domain/interaction_state.h"
#include <algorithm>

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
