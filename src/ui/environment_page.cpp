#include "ui/environment_page.h"
#include "domain/models.h"
#include "platform/encoding.h"
#include "resources.h"
#include <iterator>
#include <winhttp.h>

namespace {
struct Page {
    EnvironmentSettings& data;
    std::string editing;
    bool adding;
    SaveEnvironments persist;
    HFONT heading{};
};

constexpr COLORREF ink=RGB(31,41,55), muted=RGB(107,114,128), accent=RGB(37,99,235);
constexpr COLORREF rail=RGB(247,248,250), border=RGB(211,219,232), paper=RGB(255,255,255);

RECT units(HWND page,int x,int y,int right,int bottom) {
    RECT rect{x,y,right,bottom};
    MapDialogRect(page,&rect);
    return rect;
}

void rounded(HDC dc,RECT rect,COLORREF fill,COLORREF edge,int radius) {
    auto oldPen=SelectObject(dc,GetStockObject(DC_PEN));
    auto oldBrush=SelectObject(dc,GetStockObject(DC_BRUSH));
    SetDCPenColor(dc,edge);
    SetDCBrushColor(dc,fill);
    RoundRect(dc,rect.left,rect.top,rect.right,rect.bottom,radius,radius);
    SelectObject(dc,oldBrush);
    SelectObject(dc,oldPen);
}

void paintPage(HWND page) {
    PAINTSTRUCT ps{};
    auto dc=BeginPaint(page,&ps);
    FillRect(dc,&ps.rcPaint,GetSysColorBrush(COLOR_WINDOW));
    auto sidebar=units(page,0,0,116,198);
    SetDCBrushColor(dc,rail);
    FillRect(dc,&sidebar,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    auto radius=units(page,0,0,5,0).right;
    for(int id:{IDC_ENV_NAME,IDC_ENV_HOST}){
        auto edit=GetDlgItem(page,id);
        RECT rect{};
        GetWindowRect(edit,&rect);
        MapWindowPoints(nullptr,page,reinterpret_cast<POINT*>(&rect),2);
        auto padding=units(page,0,0,6,4);
        InflateRect(&rect,padding.right,padding.bottom);
        rounded(dc,rect,paper,GetFocus()==edit?accent:border,radius);
    }
    EndPaint(page,&ps);
}

void drawControl(HWND page,const DRAWITEMSTRUCT& item,const Page& state) {
    auto dc=item.hDC;
    int saved=SaveDC(dc);
    auto rect=item.rcItem;
    bool row=item.CtlType==ODT_LISTBOX;
    bool selected=item.itemState&ODS_SELECTED;
    bool primary=item.CtlID==IDOK;
    COLORREF color=ink;
    std::wstring label;
    if(row){
        SetDCBrushColor(dc,rail);
        FillRect(dc,&rect,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        if(item.itemID==static_cast<UINT>(-1)){RestoreDC(dc,saved);return;}
        if(selected)rounded(dc,rect,RGB(234,242,255),RGB(234,242,255),units(page,0,0,5,0).right);
        auto length=SendMessageW(item.hwndItem,LB_GETTEXTLEN,item.itemID,0);
        label.resize(length+1);
        SendMessageW(item.hwndItem,LB_GETTEXT,item.itemID,reinterpret_cast<LPARAM>(label.data()));
        label.resize(length);
        InflateRect(&rect,-units(page,0,0,6,0).right,0);
        if(selected)color=accent;
        if(label==toWide(state.data.activeEnvironment)){
            auto dot=units(page,0,0,4,0).right;
            int top=(rect.top+rect.bottom-dot)/2;
            rounded(dc,{rect.right-dot,top,rect.right,top+dot},accent,accent,dot);
            rect.right-=dot*2;
        }
    }else{
        wchar_t caption[64]{};
        GetWindowTextW(item.hwndItem,caption,64);
        label=caption;
        auto fill=primary?(selected?RGB(29,78,216):accent):(selected?RGB(238,242,247):paper);
        rounded(dc,rect,fill,primary?fill:border,units(page,0,0,5,0).right);
        color=primary?paper:(item.CtlID==IDC_ENV_DELETE?RGB(180,56,66):ink);
        if(item.itemState&ODS_DISABLED)color=RGB(161,168,180);
    }
    SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,color);
    DrawTextW(dc,label.c_str(),-1,&rect,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX|(row?DT_LEFT:DT_CENTER));
    if((item.itemState&ODS_FOCUS)&&!(item.itemState&ODS_NOFOCUSRECT)){
        InflateRect(&rect,-2,-2);
        DrawFocusRect(dc,&rect);
    }
    RestoreDC(dc,saved);
}

std::wstring field(HWND page,int id) {
    HWND edit=GetDlgItem(page,id);
    std::wstring value(GetWindowTextLengthW(edit)+1,L'\0');
    int length=GetWindowTextW(edit,value.data(),static_cast<int>(value.size()));
    value.resize(length);
    auto first=value.find_first_not_of(L" \t\r\n");
    return first==value.npos?L"":value.substr(first,value.find_last_not_of(L" \t\r\n")-first+1);
}

void selectRow(HWND page,const Page& state) {
    auto list=GetDlgItem(page,IDC_ENV_LIST);
    auto index=state.editing.empty()?LB_ERR:std::distance(state.data.environmentHosts.begin(),state.data.environmentHosts.find(state.editing));
    SendMessageW(list,LB_SETCURSEL,index,0);
}

void loadForm(HWND page,Page& state,const std::string& key) {
    state.editing=key;
    SetDlgItemTextW(page,IDC_ENV_TITLE,key.empty()?L"添加环境":L"编辑环境");
    selectRow(page,state);
    SetDlgItemTextW(page,IDC_ENV_NAME,toWide(key).c_str());
    SetDlgItemTextW(page,IDC_ENV_HOST,key.empty()?L"":toWide(state.data.environmentHosts.at(key)).c_str());
    SetDlgItemTextW(page,IDC_ENV_MESSAGE,L"");
    EnableWindow(GetDlgItem(page,IDC_ENV_DELETE),!key.empty());
}

void reloadList(HWND page,const Page& state) {
    HWND list=GetDlgItem(page,IDC_ENV_LIST);
    SendMessageW(list,LB_RESETCONTENT,0,0);
    for(auto& entry:state.data.environmentHosts)
        SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(toWide(entry.first).c_str()));
}

bool discardForm(HWND page,const Page& state) {
    const auto host=state.editing.empty()?std::string{}:state.data.environmentHosts.at(state.editing);
    if(field(page,IDC_ENV_NAME)==toWide(state.editing)&&field(page,IDC_ENV_HOST)==toWide(host))return true;
    return MessageBoxW(page,L"放弃尚未保存的环境修改？",L"环境管理",MB_YESNO|MB_DEFBUTTON2)==IDYES;
}

bool validHost(const std::wstring& host) {
    URL_COMPONENTSW parts{};
    parts.dwStructSize=sizeof(parts);
    parts.dwHostNameLength=static_cast<DWORD>(-1);
    auto scheme=host.find(L"://");
    return scheme!=host.npos&&host.find_first_of(L"/?#@",scheme+3)==host.npos&&
        host.find_first_of(L" \t\r\n\\")==host.npos&&WinHttpCrackUrl(host.c_str(),0,0,&parts)&&parts.dwHostNameLength&&
        (parts.nScheme==INTERNET_SCHEME_HTTP||parts.nScheme==INTERNET_SCHEME_HTTPS);
}

bool commit(HWND page,Page& state,EnvironmentSettings next) {
    if(!state.persist(next)){
        SetDlgItemTextW(page,IDC_ENV_MESSAGE,L"保存失败，请检查目录权限后重试。");
        return false;
    }
    state.data=std::move(next);
    reloadList(page,state);
    return true;
}

void save(HWND page,Page& state) {
    auto key=toUtf8(field(page,IDC_ENV_NAME));
    auto host=field(page,IDC_ENV_HOST);
    if(!host.empty()&&host.back()==L'/')host.pop_back();
    if(key.empty()||(key!=state.editing&&state.data.environmentHosts.count(key))){
        SetDlgItemTextW(page,IDC_ENV_MESSAGE,L"请输入非空且唯一的环境名称。");
        SetFocus(GetDlgItem(page,IDC_ENV_NAME));
        return;
    }
    if(!validHost(host)){
        SetDlgItemTextW(page,IDC_ENV_MESSAGE,L"请输入 HTTP 或 HTTPS 地址，可带端口，例如 https://api.example.com:8443。");
        SetFocus(GetDlgItem(page,IDC_ENV_HOST));
        return;
    }
    auto next=state.data;
    if(state.editing.empty()||next.activeEnvironment==state.editing)next.activeEnvironment=key;
    next.environmentHosts.erase(state.editing);
    next.environmentHosts[key]=toUtf8(host);
    if(!commit(page,state,std::move(next)))return;
    loadForm(page,state,key);
    SetDlgItemTextW(page,IDC_ENV_MESSAGE,L"环境已保存。");
}

INT_PTR CALLBACK pageProc(HWND page,UINT msg,WPARAM w,LPARAM l) {
    if(msg==WM_INITDIALOG){
        auto& state=*reinterpret_cast<Page*>(l);
        SetWindowLongPtrW(page,DWLP_USER,l);
        LOGFONTW font{};
        GetObjectW(reinterpret_cast<HFONT>(SendMessageW(page,WM_GETFONT,0,0)),sizeof(font),&font);
        font.lfWeight=FW_SEMIBOLD;
        state.heading=CreateFontIndirectW(&font);
        for(int id:{IDC_ENV_TITLE,IDC_ENV_LIST_TITLE})
            SendDlgItemMessageW(page,id,WM_SETFONT,reinterpret_cast<WPARAM>(state.heading),TRUE);
        SendDlgItemMessageW(page,IDC_ENV_LIST,LB_SETITEMHEIGHT,0,units(page,0,0,0,23).bottom);
        reloadList(page,state);
        auto key=state.data.activeEnvironment;
        if(!state.data.environmentHosts.count(key)&&!state.data.environmentHosts.empty())key=state.data.environmentHosts.begin()->first;
        loadForm(page,state,state.adding||state.data.environmentHosts.empty()?std::string{}:key);
        SetFocus(GetDlgItem(page,state.editing.empty()?IDC_ENV_NAME:IDC_ENV_LIST));
        return FALSE;
    }
    auto state=reinterpret_cast<Page*>(GetWindowLongPtrW(page,DWLP_USER));
    if(!state)return FALSE;
    if(msg==WM_PAINT){paintPage(page);return TRUE;}
    if(msg==WM_DRAWITEM){drawControl(page,*reinterpret_cast<DRAWITEMSTRUCT*>(l),*state);return TRUE;}
    if(msg==WM_DESTROY){DeleteObject(state->heading);return FALSE;}
    if(msg==WM_CTLCOLORSTATIC||msg==WM_CTLCOLORLISTBOX||msg==WM_CTLCOLOREDIT){
        auto dc=reinterpret_cast<HDC>(w);
        int id=GetDlgCtrlID(reinterpret_cast<HWND>(l));
        auto background=(id==IDC_ENV_LIST_TITLE||id==IDC_ENV_LIST)?rail:paper;
        SetBkColor(dc,background);
        SetTextColor(dc,(id==IDC_ENV_MESSAGE||id==-1)?muted:ink);
        SetDCBrushColor(dc,background);
        return reinterpret_cast<INT_PTR>(GetStockObject(DC_BRUSH));
    }
    if(msg==WM_CLOSE||(msg==WM_COMMAND&&LOWORD(w)==IDCANCEL)){
        if(discardForm(page,*state))EndDialog(page,0);
        return TRUE;
    }
    if(msg!=WM_COMMAND)return FALSE;
    switch(LOWORD(w)){
    case IDC_ENV_NAME:
    case IDC_ENV_HOST:
        if(HIWORD(w)==EN_SETFOCUS||HIWORD(w)==EN_KILLFOCUS)InvalidateRect(page,nullptr,FALSE);
        return TRUE;
    case IDC_ENV_LIST:
        if(HIWORD(w)==LBN_SELCHANGE){
            auto index=SendDlgItemMessageW(page,IDC_ENV_LIST,LB_GETCURSEL,0,0);
            if(index==LB_ERR)return TRUE;
            if(!discardForm(page,*state)){selectRow(page,*state);return TRUE;}
            loadForm(page,*state,std::next(state->data.environmentHosts.begin(),index)->first);
        }
        return TRUE;
    case IDC_ENV_ADD:
        if(discardForm(page,*state)){loadForm(page,*state,{});SetFocus(GetDlgItem(page,IDC_ENV_NAME));}
        return TRUE;
    case IDOK:save(page,*state);return TRUE;
    case IDC_ENV_DELETE:
        if(!state->editing.empty()&&MessageBoxW(page,(L"删除环境“"+toWide(state->editing)+L"”？").c_str(),L"环境管理",MB_YESNO|MB_DEFBUTTON2)==IDYES){
            auto next=state->data;
            if(state->editing==next.activeEnvironment)next.activeEnvironment.clear();
            next.environmentHosts.erase(state->editing);
            if(!commit(page,*state,std::move(next)))return TRUE;
            auto& hosts=state->data.environmentHosts;
            loadForm(page,*state,hosts.empty()?std::string{}:hosts.begin()->first);
        }
        return TRUE;
    }
    return FALSE;
}
}

void showEnvironmentPage(HINSTANCE instance,HWND owner,EnvironmentSettings& data,bool adding,SaveEnvironments persist) {
    Page state{data,{},adding,persist};
    DialogBoxParamW(instance,MAKEINTRESOURCEW(IDD_ENV_PAGE),owner,pageProc,reinterpret_cast<LPARAM>(&state));
}
