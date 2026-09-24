#include "ui/main_window.h"
#include "ui/ui_helpers.h"
#include "domain/interaction_state.h"
#include "application/openapi_import.h"
#include "application/request_operations.h"
#include "infrastructure/http_client.h"
#include "json/json.h"
#include "platform/encoding.h"
#include "platform/identity.h"
#include "resources.h"

#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shlwapi.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_set>


using std::string;
using std::wstring;

namespace {
enum : int {
    IDC_SEARCH=100,IDC_ADD_FOLDER,IDC_TREE,IDC_REQUEST_TABS,IDC_METHOD,IDC_URL,IDC_SAVE,IDC_SAVE_MORE,
    IDC_SEND,IDC_CANCEL,IDC_EDITOR_TABS,IDC_KV_LIST,IDC_ADD_ROW,IDC_DELETE_ROW,IDC_BODY_TYPE,
    IDC_FORMAT,IDC_COMPRESS,IDC_BODY,IDC_VALIDATION,IDC_SUMMARY,
    IDC_RESPONSE_TABS,IDC_RESPONSE_BODY,IDC_RESPONSE_HEADERS,IDC_RESPONSE_FIND_PANEL,IDC_RESPONSE_FIND_EDIT,IDC_RESPONSE_FIND_PREV,IDC_RESPONSE_FIND_NEXT,IDC_RESPONSE_FIND_CLOSE,IDC_BODY_NONE,IDC_EMPTY_TITLE,IDC_EMPTY_HELP,IDC_SIDEBAR_DIVIDER,IDC_REQUEST_TAB_SCROLL,
    IDC_RESPONSE_MODE,IDC_RESPONSE_FIND,IDC_FIND_STATUS,IDC_RESPONSE_MODE_DIVIDER,
    IDC_PROMPT_EDIT=900,IDC_URL_FRAME,
    IDM_FOLDER_ADD_REQUEST=1000,IDM_FOLDER_ADD_CHILD,IDM_FOLDER_IMPORT,IDM_FOLDER_RENAME,IDM_FOLDER_DELETE,
    IDM_REQUEST_OPEN,IDM_REQUEST_RENAME,IDM_REQUEST_DUPLICATE,IDM_REQUEST_MOVE,IDM_REQUEST_DELETE,
    IDM_SAVE_CASE,IDM_CASE_DELETE,
    IDM_TAB_CLOSE_ALL,IDM_TAB_CLOSE_OTHERS,IDM_TAB_CLOSE_LEFT,IDM_TAB_CLOSE_RIGHT
};
constexpr UINT WM_HTTP_DONE=WM_APP+1;
constexpr UINT WM_SEARCH_REFRESH=WM_APP+2;
constexpr UINT WM_IMPORT_DONE=WM_APP+3;
constexpr UINT WM_EDIT_ENTRY_CELL=WM_APP+4;
constexpr UINT WM_UPDATE_TAB_SCROLL=WM_APP+5;
constexpr UINT_PTR SAVE_FEEDBACK_TIMER=1;
constexpr UINT_PTR REQUEST_TIMER=2;
constexpr UINT_PTR DIRTY_TIMER=3;
constexpr UINT_PTR FIND_WRAP_TIMER=4;
constexpr int SECTION_TAB_WIDTH=68;
constexpr int REQUEST_TAB_HEIGHT=44;
constexpr int ENTRY_ROW_HEIGHT=32;
constexpr int ENTRY_HEADER_HEIGHT=32;
constexpr int REQUEST_BAR_TOP=60;
constexpr int REQUEST_BAR_HEIGHT=40;
constexpr int REQUEST_EDITOR_TOP=REQUEST_BAR_TOP+REQUEST_BAR_HEIGHT+10;

struct NodeRef { enum class Kind { Folder, Request, Case } kind; void* value; ApiFolder* ownerFolder; ApiRequest* ownerRequest; };
struct TabState {
    ApiRequest* request=nullptr;
    ApiRequestCase* requestCase=nullptr;
    std::unique_ptr<ApiRequest> caseSnapshot;
    std::atomic<bool> cancel=false;
    std::atomic<HINTERNET> activeRequest=nullptr;
    std::atomic<bool> sending=false;
    std::thread worker;
    string responseRaw,responsePretty;
    std::vector<KeyValueEntry> responseHeaders;
    wstring summary=L"暂无响应";
    wstring validation;
    wstring responseSummary=L"暂无响应";
    bool oldResponse=false,rawView=false,findVisible=false,displayedDirty=false;
    unsigned statusCode=0;
    int editorPage=0,responsePage=0;
    wstring findQuery,findStatus;
    size_t findPosition=wstring::npos;
    DWORD selectionStart=0,selectionEnd=0;
    int firstVisibleLine=0,horizontalScroll=0;
    POINT responseScroll{};
    std::chrono::steady_clock::time_point started;
    void cancelNow(){cancel=true;HINTERNET handle=activeRequest.exchange(nullptr);if(handle)WinHttpCloseHandle(handle);}
    ~TabState(){cancelNow();if(worker.joinable())worker.join();}
};
struct HttpCompletion { std::shared_ptr<TabState> tab;HttpResult result; };
struct ImportDownload { string targetId;string sourceUrl;HttpResult result; };
struct FolderChoice { ApiFolder* folder=nullptr;std::wstring label; };
struct FolderPickerContext { const std::vector<FolderChoice>* choices=nullptr;ApiFolder* selected=nullptr;bool accepted=false; };

MainWindowServices gServices{};
HINSTANCE gInstance{};
HMODULE gRichEditModule{};
HWND gWindow{},gSearch{},gAddFolder{},gTree{},gSidebarDivider{},gRequestTabs{},gRequestTabScroll{},gMethod{},gUrlFrame{},gUrl{},gSave{},gSaveMore{},gSend{},gCancel{},gSaveTooltip{};
HWND gEditorTabs{},gKvList{},gBodyType{},gFormat{},gCompress{},gBody{},gValidation{};
HWND gSummary{},gResponseTabs{},gResponseBody{},gResponseHeaders{},gResponseFindPanel{},gResponseFindEdit{},gResponseFindPrev{},gResponseFindNext{},gResponseFindClose{},gBodyNone{},gEmptyTitle{},gEmptyHelp{};
HWND gResponseMode{},gResponseModeDivider{},gResponseFind{},gFindStatus{},gResponseToolbarHot{},gRequestToolbarHot{};
HFONT gUiFont{},gCodeFont{},gTitleFont{},gTreeFolderFont{},gTreeMethodFont{},gEntryTypeFont{};
HBRUSH gSidebarBrush{},gWhiteBrush{};
HBRUSH gAccentBrush{},gSelectedBrush{},gHoverBrush{},gBorderBrush{};
AppData gData;
WorkspaceContent gSavedContent;
bool gSaveFailed=false,gClosingPrompt=false;
std::vector<std::pair<UINT,WPARAM>> gDeferredCompletions;
std::weak_ptr<TabState> gDisplayedTab;
std::vector<std::unique_ptr<NodeRef>> gNodeRefs;
std::vector<std::shared_ptr<TabState>> gTabs;
int gSelectedTab=-1;
int gSelectedEntryRows[3]{-1,-1,-1};
ApiFolder* gSelectedFolder=nullptr;
NodeRef* gTreeSelection=nullptr;
int gEditorPage=0,gResponsePage=0;
int gEditorTabHot=-1,gResponseTabHot=-1;
int gRequestTabHot=-1,gRequestTabCloseHot=-1;
int gEntryHotRow=-1,gEntryHotColumn=-1;
bool gLoadingEditor=false,gLoadingEntryList=false;
ApiRequest* gDragRequest=nullptr;
ApiFolder* gDragTarget=nullptr;
HTREEITEM gDragHover=nullptr;
HWND gCellEditor=nullptr;
int gEditRow=-1,gEditColumn=-1;
int gFocusedEntryColumn=1;
bool gImeComposing=false;
bool gResizeSidebar=false,gResizePanels=false;
std::atomic<bool> gImportCancel=false;
bool gImporting=false;
wstring gSaveStatus=L"已保存";
std::thread gImportWorker;
std::atomic<HINTERNET> gImportRequest=nullptr;
bool gSelectingTree=false;
bool gRebuildingTree=false;
bool gDraggingRequestTabScroll=false;
int gRequestTabScrollDragOffset=0;
int gRequestTabScrollOffset=0,gRequestTabContentWidth=0,gRequestTabViewportX=0,gRequestTabViewportWidth=0;
bool gEnsureSelectedRequestTabVisible=false;
bool gResponseFindVisible=false;
wstring gResponseFindQuery;
size_t gResponseFindPosition=wstring::npos;
wstring gResponseSearchText;
std::vector<size_t> gResponseMatches,gResponseHighlights;
size_t gResponseHighlightLength=0,gResponseHighlightCurrent=wstring::npos;
bool gApplyingResponseHighlights=false,gResponseFindWrapped=false;
UINT gDpi=96;
std::unordered_set<string> gExpandedRequests;

void commitCellEditor(bool save=true);
void closeTab(int index,bool confirm=true);
void closeTabRange(int contextIndex,int command);
void layout(int width,int height);
void findInResponseBody(bool forward);
void showResponseFind(bool visible);
void refreshResponseFind(bool selectFirst);
void paintResponseMatches();
void layoutResponseFindControls();
void refreshRequestTabs();
void updateDirtyStatus();
void captureView();
void moveFocus(HWND from,bool backwards);

void setText(HWND control,const wstring& value){SetWindowTextW(control,value.c_str());}
wstring textOf(HWND control){int n=GetWindowTextLengthW(control);wstring value((size_t)n+1,L'\0');GetWindowTextW(control,value.data(),n+1);value.resize(n);return value;}
wstring trimWide(wstring value){while(!value.empty()&&iswspace(value.front()))value.erase(value.begin());while(!value.empty()&&iswspace(value.back()))value.pop_back();return value;}
void applyFont(HWND control,HFONT font=nullptr){SendMessageW(control,WM_SETFONT,(WPARAM)(font?font:gUiFont),TRUE);}
void setVisible(HWND control,bool visible){ShowWindow(control,visible?SW_SHOW:SW_HIDE);}
void setValidationText(const wstring& value){setText(gValidation,value);setVisible(gValidation,!value.empty());if(gWindow){RECT client{};GetClientRect(gWindow,&client);layout((int)client.right,(int)client.bottom);}}
std::shared_ptr<TabState> selectedTab(){return gSelectedTab>=0&&gSelectedTab<(int)gTabs.size()?gTabs[(size_t)gSelectedTab]:nullptr;}
int px(int value){return MulDiv(value,(int)gDpi,96);}
int dip(int value){return MulDiv(value,96,(int)gDpi);}
void updateSearchFormatting() {
    if(!gSearch)return;
    RECT client{};GetClientRect(gSearch,&client);HDC dc=GetDC(gSearch);auto oldFont=(HFONT)SelectObject(dc,gUiFont);TEXTMETRICW metrics{};GetTextMetricsW(dc,&metrics);SelectObject(dc,oldFont);ReleaseDC(gSearch,dc);
    int clientRight=(int)client.right;int clientBottom=(int)client.bottom;int textHeight=(int)metrics.tmHeight+px(1);int top=std::max(0,(clientBottom-textHeight)/2);RECT formatting{px(28),top,std::max(px(29),clientRight-px(8)),std::min(clientBottom,top+textHeight)};
    SendMessageW(gSearch,EM_SETRECTNP,0,(LPARAM)&formatting);InvalidateRect(gSearch,nullptr,TRUE);
}
void recreateFonts() {
    if(gUiFont)DeleteObject(gUiFont);if(gCodeFont)DeleteObject(gCodeFont);if(gTitleFont)DeleteObject(gTitleFont);if(gTreeFolderFont)DeleteObject(gTreeFolderFont);if(gTreeMethodFont)DeleteObject(gTreeMethodFont);if(gEntryTypeFont)DeleteObject(gEntryTypeFont);
    gUiFont=CreateFontW(-px(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    gCodeFont=CreateFontW(-px(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
    gTitleFont=CreateFontW(-px(21),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    gTreeFolderFont=CreateFontW(-px(13),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    gTreeMethodFont=CreateFontW(-px(10),0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    gEntryTypeFont=CreateFontW(-px(11),0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
}
int fontTextHeight(HWND control,HFONT font) {
    HDC dc=GetDC(control);HGDIOBJ oldFont=SelectObject(dc,font);TEXTMETRICW metrics{};GetTextMetricsW(dc,&metrics);SelectObject(dc,oldFont);ReleaseDC(control,dc);return (int)metrics.tmHeight;
}
void updateUrlFormatting() {
    if(!gUrl)return;
    RECT client{};GetClientRect(gUrl,&client);int textHeight=fontTextHeight(gUrl,gCodeFont)+px(1);int top=std::max(0,((int)client.bottom-textHeight)/2);
    RECT formatting{0,top,(int)client.right,std::min((int)client.bottom,top+textHeight)};SendMessageW(gUrl,EM_SETRECTNP,0,(LPARAM)&formatting);InvalidateRect(gUrl,nullptr,FALSE);
}
void setSaveStatus(const wstring& value) {
    if(value==L"未保存"){SetTimer(gWindow,DIRTY_TIMER,120,nullptr);return;}
    gSaveStatus=value;if(gSave)InvalidateRect(gSave,nullptr,FALSE);
}
void showSaveFeedback(bool success) {
    if(!gSave||!gWindow)return;
    KillTimer(gWindow,SAVE_FEEDBACK_TIMER);
    setText(gSave,success?L"已保存":L"保存失败");
    InvalidateRect(gSave,nullptr,TRUE);
    if(success)SetTimer(gWindow,SAVE_FEEDBACK_TIMER,1600,nullptr);
}

CaseContent caseContent(const TabState& tab) {
    return {RequestContent::from(*tab.caseSnapshot),toUtf8(tab.responseSummary),tab.responseRaw,tab.responsePretty,tab.responseHeaders};
}
bool tabDirty(const TabState& tab) {
    if(tab.requestCase){auto found=gSavedContent.cases.find(tab.requestCase->id);return found==gSavedContent.cases.end()||!(found->second==caseContent(tab));}
    auto found=gSavedContent.requests.find(tab.request->id);
    return found==gSavedContent.requests.end()||!(found->second==RequestContent::from(*tab.request));
}
bool workspaceDirty() {
    for(const auto& tab:gTabs)if(tabDirty(*tab))return true;
    return !(gSavedContent==WorkspaceContent::from(gData));
}
void updateDirtyStatus() {
    bool changed=false;for(auto& tab:gTabs){bool dirty=tabDirty(*tab);if(dirty!=tab->displayedDirty){tab->displayedDirty=dirty;changed=true;}}
    if(changed)refreshRequestTabs();
    gSaveStatus=gSaveFailed?L"保存失败，请重试":(workspaceDirty()?L"未保存":L"已保存");
    if(gSaveStatus==L"未保存"&&textOf(gSave)==L"已保存"){KillTimer(gWindow,SAVE_FEEDBACK_TIMER);setText(gSave,L"保存");}
    if(gSave)InvalidateRect(gSave,nullptr,FALSE);
}

int askSaveChanges(const wstring& title,const wstring& message,bool exiting=false) {
    TASKDIALOG_BUTTON buttons[]={{IDYES,exiting?L"保存全部并退出":L"保存全部并关闭"},
        {IDNO,exiting?L"放弃修改并退出":L"放弃目标标签修改"},{IDCANCEL,L"取消"}};
    TASKDIALOGCONFIG config{};config.cbSize=sizeof(config);config.hwndParent=gWindow;
    config.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION|TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle=L"FeatherApi";config.pszMainInstruction=title.c_str();config.pszContent=message.c_str();
    config.cButtons=3;config.pButtons=buttons;config.nDefaultButton=IDCANCEL;config.pszMainIcon=TD_WARNING_ICON;
    int choice=IDCANCEL;gClosingPrompt=true;
    HRESULT result=gServices.showTaskDialog(&config,&choice,nullptr,nullptr);
    if(FAILED(result))choice=MessageBoxW(gWindow,(message+L"\r\n\r\n是：保存全部\r\n否：放弃修改\r\n取消：返回编辑").c_str(),title.c_str(),MB_YESNOCANCEL|MB_ICONWARNING|MB_DEFBUTTON3);
    gClosingPrompt=false;for(auto pending:gDeferredCompletions)PostMessageW(gWindow,pending.first,pending.second,0);gDeferredCompletions.clear();return choice;
}

constexpr COLORREF COLOR_ACCENT=RGB(37,99,235);
constexpr COLORREF COLOR_PRIMARY=RGB(31,41,55);
constexpr COLORREF COLOR_SECONDARY=RGB(107,114,128);
constexpr COLORREF COLOR_BORDER=RGB(229,231,235);
constexpr COLORREF COLOR_SIDEBAR=RGB(247,248,250);
constexpr COLORREF COLOR_SELECTED=RGB(234,242,255);
constexpr COLORREF COLOR_HOVER=RGB(238,242,247);
constexpr COLORREF COLOR_SELECTED_BORDER=RGB(199,218,255);

void fillRoundRect(HDC dc,const RECT& rect,COLORREF color,int radius) {
    HBRUSH brush=CreateSolidBrush(color);HPEN pen=CreatePen(PS_SOLID,1,color);
    auto oldBrush=(HBRUSH)SelectObject(dc,brush);auto oldPen=(HPEN)SelectObject(dc,pen);
    RoundRect(dc,rect.left,rect.top,rect.right,rect.bottom,px(radius),px(radius));
    SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(pen);DeleteObject(brush);
}

void drawFolderGlyph(HDC dc,RECT rect,bool open,bool module) {
    COLORREF color=module?COLOR_ACCENT:COLOR_SECONDARY;
    HBRUSH brush=CreateSolidBrush(color);HPEN pen=CreatePen(PS_SOLID,1,color);
    auto oldBrush=(HBRUSH)SelectObject(dc,brush);auto oldPen=(HPEN)SelectObject(dc,pen);
    if(module){
        POINT points[]={{rect.left+px(7),rect.top},{rect.right,rect.top+px(4)},{rect.right,rect.bottom-px(4)},{rect.left+px(7),rect.bottom},{rect.left,rect.bottom-px(4)},{rect.left,rect.top+px(4)}};
        Polygon(dc,points,6);
        HPEN detail=CreatePen(PS_SOLID,1,COLOR_SIDEBAR);SelectObject(dc,detail);
        MoveToEx(dc,rect.left+px(2),rect.top+px(5),nullptr);LineTo(dc,rect.left+px(7),rect.top+px(8));LineTo(dc,rect.right-px(2),rect.top+px(5));
        MoveToEx(dc,rect.left+px(7),rect.top+px(8),nullptr);LineTo(dc,rect.left+px(7),rect.bottom-px(2));
        SelectObject(dc,oldPen);DeleteObject(detail);oldPen=nullptr;
    }else{
        POINT folder[]={{rect.left,rect.top+px(3)},{rect.left+px(6),rect.top+px(3)},{rect.left+px(8),rect.top+px(5)},{rect.right,rect.top+px(5)},{rect.right-(open?px(2):0),rect.bottom},{rect.left,rect.bottom}};
        Polygon(dc,folder,6);
        if(open){
            HBRUSH background=CreateSolidBrush(COLOR_SIDEBAR);SelectObject(dc,background);
            POINT cut[]={{rect.left+px(2),rect.top+px(7)},{rect.right,rect.top+px(7)},{rect.right-px(3),rect.bottom},{rect.left,rect.bottom}};Polygon(dc,cut,4);
            SelectObject(dc,brush);DeleteObject(background);
        }
    }
    if(oldPen)SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(pen);DeleteObject(brush);
}

int treeItemDepth(HTREEITEM item) {
    int depth=0;
    while((item=TreeView_GetParent(gTree,item))!=nullptr)++depth;
    return depth;
}

int treeItemContentLeft(HTREEITEM item) {
    constexpr int contentLeft=26;
    constexpr int levelIndent=20;
    return px(contentLeft+treeItemDepth(item)*levelIndent);
}

LRESULT drawTreeItem(const NMTVCUSTOMDRAW* draw) {
    if(draw->nmcd.dwDrawStage==CDDS_PREPAINT)return CDRF_NOTIFYITEMDRAW;
    if(draw->nmcd.dwDrawStage!=CDDS_ITEMPREPAINT)return CDRF_DODEFAULT;
    HTREEITEM item=(HTREEITEM)draw->nmcd.dwItemSpec;TVITEMW info{};info.mask=TVIF_PARAM|TVIF_STATE;info.stateMask=TVIS_EXPANDED;info.hItem=item;
    if(!TreeView_GetItem(gTree,&info))return CDRF_DODEFAULT;
    auto ref=(NodeRef*)info.lParam;if(!ref)return CDRF_DODEFAULT;
    RECT itemRect{};if(!TreeView_GetItemRect(gTree,item,&itemRect,FALSE))return CDRF_DODEFAULT;
    RECT client{};GetClientRect(gTree,&client);RECT paintRow{client.left,itemRect.top,client.right,itemRect.bottom};RECT row{px(2),itemRect.top+px(1),client.right-px(2),itemRect.bottom-px(1)};
    // Keep the update-region clip installed by the tree view.  Clearing it
    // here allowed a row repaint during live resize to draw outside the part
    // Windows was currently updating, leaving copies of the rounded selection
    // edge behind as the control grew and shrank.
    int savedDc=SaveDC(draw->nmcd.hdc);IntersectClipRect(draw->nmcd.hdc,paintRow.left,paintRow.top,paintRow.right,paintRow.bottom);
    bool folder=ref->kind==NodeRef::Kind::Folder;bool selected=(draw->nmcd.uItemState&CDIS_SELECTED)!=0;bool hot=(draw->nmcd.uItemState&CDIS_HOT)!=0;
    FillRect(draw->nmcd.hdc,&paintRow,gSidebarBrush);
    if(!folder&&(selected||hot)){
        fillRoundRect(draw->nmcd.hdc,row,selected?COLOR_SELECTED:COLOR_HOVER,7);
        if(selected){HBRUSH border=CreateSolidBrush(COLOR_SELECTED_BORDER);HRGN region=CreateRoundRectRgn(row.left,row.top,row.right,row.bottom,px(7),px(7));FrameRgn(draw->nmcd.hdc,region,border,1,1);DeleteObject(region);DeleteObject(border);}
    }
    SetBkMode(draw->nmcd.hdc,TRANSPARENT);int x=treeItemContentLeft(item);int center=(itemRect.top+itemRect.bottom)/2;
    bool hasChildren=TreeView_GetChild(gTree,item)!=nullptr;
    if(hasChildren){
        POINT arrow[3];bool expanded=(info.state&TVIS_EXPANDED)!=0;
        if(expanded){arrow[0]={x-px(15),center-px(3)};arrow[1]={x-px(7),center-px(3)};arrow[2]={x-px(11),center+px(2)};}
        else{arrow[0]={x-px(13),center-px(4)};arrow[1]={x-px(8),center};arrow[2]={x-px(13),center+px(4)};}
        HBRUSH arrowBrush=CreateSolidBrush(COLOR_SECONDARY);auto old=(HBRUSH)SelectObject(draw->nmcd.hdc,arrowBrush);Polygon(draw->nmcd.hdc,arrow,3);SelectObject(draw->nmcd.hdc,old);DeleteObject(arrowBrush);
    }
    RECT textRect{x,itemRect.top,client.right-px(10),itemRect.bottom};HFONT font=gUiFont;wstring text;
    if(folder){
        auto value=(ApiFolder*)ref->value;bool module=TreeView_GetParent(gTree,item)==nullptr;RECT glyph{textRect.left,center-px(7),textRect.left+px(15),center+px(7)};
        drawFolderGlyph(draw->nmcd.hdc,glyph,(info.state&TVIS_EXPANDED)!=0,module);textRect.left+=px(21);font=gTreeFolderFont;text=toWide(value->name);
        auto old=(HFONT)SelectObject(draw->nmcd.hdc,font);SetTextColor(draw->nmcd.hdc,COLOR_PRIMARY);DrawTextW(draw->nmcd.hdc,text.c_str(),-1,&textRect,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
        if(!module){SIZE nameSize{};GetTextExtentPoint32W(draw->nmcd.hdc,text.c_str(),(int)text.size(),&nameSize);RECT countRect=textRect;countRect.left=std::min(textRect.right,textRect.left+nameSize.cx+px(5));SelectObject(draw->nmcd.hdc,gUiFont);SetTextColor(draw->nmcd.hdc,COLOR_SECONDARY);wstring count=L"("+std::to_wstring(value->requests.size())+L")";DrawTextW(draw->nmcd.hdc,count.c_str(),-1,&countRect,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);}
        SelectObject(draw->nmcd.hdc,old);
    }else if(ref->kind==NodeRef::Kind::Request){
        auto request=(ApiRequest*)ref->value;wstring method=toWide(request->method);auto old=(HFONT)SelectObject(draw->nmcd.hdc,gTreeMethodFont);SetTextColor(draw->nmcd.hdc,COLOR_ACCENT);
        RECT methodRect=textRect;DrawTextW(draw->nmcd.hdc,method.c_str(),-1,&methodRect,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_CALCRECT);methodRect.top=textRect.top;methodRect.bottom=textRect.bottom;DrawTextW(draw->nmcd.hdc,method.c_str(),-1,&methodRect,DT_SINGLELINE|DT_VCENTER|DT_LEFT);
        textRect.left=methodRect.right+px(10);SelectObject(draw->nmcd.hdc,gUiFont);SetTextColor(draw->nmcd.hdc,COLOR_PRIMARY);text=toWide(request->name);DrawTextW(draw->nmcd.hdc,text.c_str(),-1,&textRect,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);SelectObject(draw->nmcd.hdc,old);
    }else{
        auto requestCase=(ApiRequestCase*)ref->value;SetTextColor(draw->nmcd.hdc,RGB(229,159,0));RECT diamond{textRect.left,center-px(4),textRect.left+px(8),center+px(4)};POINT points[]={{(diamond.left+diamond.right)/2,diamond.top},{diamond.right,(diamond.top+diamond.bottom)/2},{(diamond.left+diamond.right)/2,diamond.bottom},{diamond.left,(diamond.top+diamond.bottom)/2}};HBRUSH brush=CreateSolidBrush(RGB(229,159,0));auto oldBrush=(HBRUSH)SelectObject(draw->nmcd.hdc,brush);Polygon(draw->nmcd.hdc,points,4);SelectObject(draw->nmcd.hdc,oldBrush);DeleteObject(brush);
        textRect.left+=px(18);auto old=(HFONT)SelectObject(draw->nmcd.hdc,gUiFont);SetTextColor(draw->nmcd.hdc,COLOR_PRIMARY);text=toWide(requestCase->name);DrawTextW(draw->nmcd.hdc,text.c_str(),-1,&textRect,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);SelectObject(draw->nmcd.hdc,old);
    }
    RestoreDC(draw->nmcd.hdc,savedDc);return CDRF_SKIPDEFAULT;
}

RECT requestTabCardRect(HWND tab,int index) {
    RECT rect{};TabCtrl_GetItemRect(tab,index,&rect);
    rect.left+=px(3);rect.right-=px(3);rect.top=px(5);rect.bottom=px(REQUEST_TAB_HEIGHT-5);
    return rect;
}

RECT requestTabCloseRect(HWND tab,int index) {
    RECT rect=requestTabCardRect(tab,index);int center=(rect.top+rect.bottom)/2;
    return {rect.right-px(27),center-px(11),rect.right-px(5),center+px(11)};
}

void paintRequestTabs(HWND tab,HDC dc) {
    RECT client{};GetClientRect(tab,&client);FillRect(dc,&client,gSidebarBrush);
    auto oldFont=(HFONT)SelectObject(dc,gUiFont);SetBkMode(dc,TRANSPARENT);
    int selected=TabCtrl_GetCurSel(tab);bool enabled=IsWindowEnabled(tab)!=FALSE;
    for(int index=0;index<TabCtrl_GetItemCount(tab)&&index<(int)gTabs.size();++index){
        RECT card=requestTabCardRect(tab,index);if(!RectVisible(dc,&card))continue;
        bool active=index==selected,hot=enabled&&index==gRequestTabHot;
        if(active){
            fillRoundRect(dc,card,RGB(255,255,255),9);
            HBRUSH border=CreateSolidBrush(COLOR_SELECTED_BORDER);
            HRGN outline=CreateRoundRectRgn(card.left,card.top,card.right,card.bottom,px(9),px(9));
            FrameRgn(dc,outline,border,px(1),px(1));DeleteObject(outline);DeleteObject(border);
        }else if(hot)fillRoundRect(dc,card,COLOR_HOVER,9);
        else if(index+1!=selected){
            RECT divider{card.right+px(2),card.top+px(9),card.right+px(3),card.bottom-px(9)};
            FillRect(dc,&divider,gBorderBrush);
        }
        const auto& state=*gTabs[(size_t)index];const auto& request=state.caseSnapshot?*state.caseSnapshot:*state.request;
        COLORREF methodColor=COLOR_SECONDARY,methodBackground=RGB(235,238,243);
        if(request.method=="GET"){methodColor=RGB(21,128,91);methodBackground=RGB(225,244,235);}
        else if(request.method=="POST"){methodColor=RGB(161,98,7);methodBackground=RGB(254,243,210);}
        else if(request.method=="PUT"){methodColor=RGB(37,99,235);methodBackground=RGB(231,239,255);}
        else if(request.method=="PATCH"){methodColor=RGB(124,58,180);methodBackground=RGB(241,232,252);}
        else if(request.method=="DELETE"){methodColor=RGB(190,55,65);methodBackground=RGB(253,233,234);}
        wstring method=toWide(request.method);SelectObject(dc,gTreeMethodFont);SIZE methodSize{};
        GetTextExtentPoint32W(dc,method.c_str(),(int)method.size(),&methodSize);
        int center=(card.top+card.bottom)/2;
        RECT badge{card.left+px(11),center-px(9),card.left+px(23)+methodSize.cx,center+px(9)};
        fillRoundRect(dc,badge,methodBackground,5);SetTextColor(dc,enabled?methodColor:COLOR_SECONDARY);
        DrawTextW(dc,method.c_str(),-1,&badge,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        RECT close=requestTabCloseRect(tab,index);
        RECT name{badge.right+px(8),card.top,close.left-px(14),card.bottom};
        SelectObject(dc,active?gTreeFolderFont:gUiFont);SetTextColor(dc,!enabled?COLOR_SECONDARY:(active?COLOR_PRIMARY:RGB(75,85,99)));
        wstring title=toWide(request.name);DrawTextW(dc,title.c_str(),-1,&name,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        // Reserve space for the dirty marker, so editing never shifts the tabs.
        if(state.displayedDirty){RECT dot{close.left-px(9),center-px(2),close.left-px(5),center+px(2)};fillRoundRect(dc,dot,COLOR_ACCENT,4);}
        bool closeHot=enabled&&index==gRequestTabCloseHot;
        if(closeHot)fillRoundRect(dc,close,RGB(222,229,239),6);
        HPEN pen=CreatePen(PS_SOLID,px(1),closeHot?COLOR_PRIMARY:RGB(148,156,169));auto oldPen=(HPEN)SelectObject(dc,pen);
        int x=(close.left+close.right)/2,y=(close.top+close.bottom)/2;
        MoveToEx(dc,x-px(3),y-px(3),nullptr);LineTo(dc,x+px(3)+1,y+px(3)+1);
        MoveToEx(dc,x+px(3),y-px(3),nullptr);LineTo(dc,x-px(3)-1,y+px(3)+1);
        SelectObject(dc,oldPen);DeleteObject(pen);
        if(enabled&&GetFocus()==tab&&index==TabCtrl_GetCurFocus(tab)&&!(SendMessageW(tab,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)){
            RECT focus=card;InflateRect(&focus,-px(3),-px(3));DrawFocusRect(dc,&focus);
        }
    }
    SelectObject(dc,oldFont);
}

int& sectionTabHot(HWND tab){return tab==gEditorTabs?gEditorTabHot:gResponseTabHot;}

void paintSectionTabs(HWND tab,HDC dc) {
    RECT client{};GetClientRect(tab,&client);FillRect(dc,&client,gWhiteBrush);
    auto oldFont=(HFONT)SelectObject(dc,gUiFont);SetBkMode(dc,TRANSPARENT);
    int selected=TabCtrl_GetCurSel(tab);bool enabled=IsWindowEnabled(tab)!=FALSE;
    for(int index=0;index<TabCtrl_GetItemCount(tab);++index){
        RECT item{};TabCtrl_GetItemRect(tab,index,&item);item.top=client.top;item.bottom=client.bottom;
        if(enabled&&index==sectionTabHot(tab)){RECT hover=item;InflateRect(&hover,-px(3),-px(4));fillRoundRect(dc,hover,COLOR_HOVER,6);}
        wchar_t label[64]{};TCITEMW info{};info.mask=TCIF_TEXT;info.pszText=label;info.cchTextMax=64;TabCtrl_GetItem(tab,index,&info);
        RECT text=item;text.bottom-=px(2);SetTextColor(dc,!enabled?RGB(156,163,175):(index==selected?COLOR_ACCENT:COLOR_SECONDARY));
        DrawTextW(dc,label,-1,&text,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if(index==selected){
            SIZE size{};GetTextExtentPoint32W(dc,label,lstrlenW(label),&size);
            int center=(item.left+item.right)/2;RECT underline{center-size.cx/2,client.bottom-px(2),center+(size.cx+1)/2,client.bottom};
            FillRect(dc,&underline,enabled?gAccentBrush:gBorderBrush);
        }
        if(enabled&&GetFocus()==tab&&index==TabCtrl_GetCurFocus(tab)&&!(SendMessageW(tab,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)){
            RECT focus=item;InflateRect(&focus,-px(4),-px(5));DrawFocusRect(dc,&focus);
        }
    }
    SelectObject(dc,oldFont);
}

LRESULT CALLBACK sectionTabsProc(HWND h,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR) {
    // Tab controls do not support NM_CUSTOMDRAW. Paint the strip ourselves,
    // while keeping the native control's selection, keyboard and accessibility.
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PRINTCLIENT){paintSectionTabs(h,(HDC)w);return 0;}
    if(message==WM_PAINT){
        PAINTSTRUCT paint{};HDC dc=BeginPaint(h,&paint);RECT client{};GetClientRect(h,&client);
        int width=client.right-client.left,height=client.bottom-client.top;
        if(width>0&&height>0){
            HDC buffer=CreateCompatibleDC(dc);HBITMAP bitmap=CreateCompatibleBitmap(dc,width,height);auto oldBitmap=SelectObject(buffer,bitmap);
            paintSectionTabs(h,buffer);BitBlt(dc,0,0,width,height,buffer,0,0,SRCCOPY);
            SelectObject(buffer,oldBitmap);DeleteObject(bitmap);DeleteDC(buffer);
        }
        EndPaint(h,&paint);return 0;
    }
    if(message==WM_MOUSEMOVE){
        TCHITTESTINFO hit{};hit.pt={(short)LOWORD(l),(short)HIWORD(l)};int hot=TabCtrl_HitTest(h,&hit);
        if(hot!=sectionTabHot(h)){sectionTabHot(h)=hot;InvalidateRect(h,nullptr,FALSE);}
        TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,h,0};TrackMouseEvent(&tracking);
    }
    if(message==WM_MOUSELEAVE||message==WM_ENABLE){sectionTabHot(h)=-1;InvalidateRect(h,nullptr,FALSE);}
    if(message==WM_NCDESTROY){sectionTabHot(h)=-1;RemoveWindowSubclass(h,sectionTabsProc,id);}
    LRESULT result=DefSubclassProc(h,message,w,l);
    if(message==WM_SETFOCUS||message==WM_KILLFOCUS||message==WM_UPDATEUISTATE||message==TCM_SETCURSEL||message==TCM_SETCURFOCUS||message==WM_KEYDOWN||message==WM_LBUTTONDOWN)
        InvalidateRect(h,nullptr,FALSE);
    return result;
}

void drawRequestControl(const DRAWITEMSTRUCT* draw) {
    HDC dc=draw->hDC;RECT rect=draw->rcItem;
    bool disabled=(draw->itemState&ODS_DISABLED)!=0,pressed=(draw->itemState&ODS_SELECTED)!=0;
    bool hot=!disabled&&gRequestToolbarHot==draw->hwndItem;
    bool method=draw->CtlID==IDC_METHOD,url=draw->CtlID==IDC_URL_FRAME;
    bool save=draw->CtlID==IDC_SAVE,more=draw->CtlID==IDC_SAVE_MORE,send=draw->CtlID==IDC_SEND;
    bool inputFocused=GetFocus()==gUrl||GetFocus()==gMethod;
    wstring label=textOf(draw->hwndItem);
    bool saved=save&&label==L"已保存",failed=save&&gSaveFailed;
    COLORREF background=RGB(255,255,255),border=RGB(218,224,233),foreground=COLOR_PRIMARY;
    if(method||url)border=inputFocused?COLOR_ACCENT:RGB(211,219,230);
    else if(send){background=disabled?RGB(225,232,244):(pressed?RGB(29,78,216):(hot?RGB(32,87,218):COLOR_ACCENT));border=background;foreground=RGB(255,255,255);}
    else if(save||more){background=pressed?RGB(235,239,245):(hot?RGB(244,246,249):RGB(255,255,255));border=hot?RGB(196,205,217):RGB(218,224,233);foreground=hot?COLOR_PRIMARY:COLOR_SECONDARY;}
    else {background=pressed?RGB(235,239,245):(hot?RGB(244,246,249):RGB(250,251,253));foreground=COLOR_SECONDARY;}
    if(saved)foreground=RGB(21,128,91);if(failed)foreground=RGB(185,55,65);
    if(disabled)foreground=RGB(156,163,175);
    FillRect(dc,&rect,(save||more)?gSidebarBrush:gWhiteBrush);
    int savedDc=SaveDC(dc);IntersectClipRect(dc,rect.left,rect.top,rect.right,rect.bottom);
    RECT outline=rect;
    // Clip adjacent native controls into a continuous rounded field or split button.
    if(method||(save&&(GetWindowLongPtrW(gSaveMore,GWL_STYLE)&WS_VISIBLE)))outline.right+=px(10);
    if(url||more)outline.left-=px(10);
    HBRUSH brush=CreateSolidBrush(background);HPEN pen=CreatePen(PS_SOLID,px(1),border);
    auto oldBrush=(HBRUSH)SelectObject(dc,brush);auto oldPen=(HPEN)SelectObject(dc,pen);
    RoundRect(dc,outline.left,outline.top,outline.right,outline.bottom,px(10),px(10));
    SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(pen);DeleteObject(brush);RestoreDC(dc,savedDc);
    if(url)return;
    int cy=(rect.top+rect.bottom)/2+(pressed?px(1):0);
    auto oldFont=(HFONT)SelectObject(dc,(method||send)?gTreeFolderFont:gUiFont);SetBkMode(dc,TRANSPARENT);
    RECT textRect=rect;UINT format=DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX;
    int saveIconCenter=0;
    if(method){
        COLORREF tint=RGB(239,242,247);foreground=COLOR_SECONDARY;
        if(label==L"GET"){foreground=RGB(21,128,91);tint=RGB(225,244,235);}
        else if(label==L"POST"){foreground=RGB(161,98,7);tint=RGB(254,243,210);}
        else if(label==L"PUT"){foreground=RGB(37,99,235);tint=RGB(231,239,255);}
        else if(label==L"PATCH"){foreground=RGB(124,58,180);tint=RGB(241,232,252);}
        else if(label==L"DELETE"){foreground=RGB(190,55,65);tint=RGB(253,233,234);}
        RECT badge{rect.left+px(7),rect.top+px(7),rect.right-px(24),rect.bottom-px(7)};
        fillRoundRect(dc,badge,disabled?COLOR_HOVER:tint,6);textRect=badge;format|=DT_CENTER;
        if(hot||pressed){RECT hover{rect.right-px(23),rect.top+px(9),rect.right-px(3),rect.bottom-px(9)};fillRoundRect(dc,hover,COLOR_HOVER,5);}
    }else if(save){
        SIZE textSize{};GetTextExtentPoint32W(dc,label.c_str(),(int)label.size(),&textSize);
        int contentWidth=px(14)+px(6)+textSize.cx;
        int contentLeft=rect.left+((rect.right-rect.left)-contentWidth)/2;
        saveIconCenter=contentLeft+px(7);textRect.left=contentLeft+px(20);textRect.right=textRect.left+textSize.cx;
        if(pressed)OffsetRect(&textRect,0,px(1));format|=DT_CENTER;
    }else if(more){
        HPEN separator=CreatePen(PS_SOLID,px(1),COLOR_BORDER);oldPen=(HPEN)SelectObject(dc,separator);
        MoveToEx(dc,rect.left,rect.top+px(7),nullptr);LineTo(dc,rect.left,rect.bottom-px(7));SelectObject(dc,oldPen);DeleteObject(separator);
    }else{
        textRect.left+=px(42);textRect.right-=px(8);format|=DT_LEFT;
        // Keep cancellation text readable when the request is waiting to stop.
        if(!save&&!send){textRect=rect;format=(format&~DT_LEFT)|DT_CENTER;}
    }
    if(disabled)foreground=RGB(156,163,175);SetTextColor(dc,foreground);
    if(!more)DrawTextW(dc,label.c_str(),-1,&textRect,format);
    pen=CreatePen(PS_SOLID,px(1),foreground);oldPen=(HPEN)SelectObject(dc,pen);oldBrush=(HBRUSH)SelectObject(dc,GetStockObject(NULL_BRUSH));
    if(method||more){
        int cx=method?rect.right-px(14):(rect.left+rect.right)/2;
        MoveToEx(dc,cx-px(3),cy-px(1),nullptr);LineTo(dc,cx,cy+px(2));LineTo(dc,cx+px(3),cy-px(1));
    }else if(save){
        int cx=saveIconCenter;
        if(saved){MoveToEx(dc,cx-px(5),cy,nullptr);LineTo(dc,cx-px(1),cy+px(4));LineTo(dc,cx+px(6),cy-px(4));}
        else if(failed){Ellipse(dc,cx-px(6),cy-px(6),cx+px(7),cy+px(7));MoveToEx(dc,cx,cy-px(3),nullptr);LineTo(dc,cx,cy+px(1));SetPixelV(dc,cx,cy+px(3),foreground);}
        else {
            POINT disk[]={{cx-px(6),cy-px(6)},{cx+px(3),cy-px(6)},{cx+px(6),cy-px(3)},{cx+px(6),cy+px(6)},{cx-px(6),cy+px(6)},{cx-px(6),cy-px(6)}};Polyline(dc,disk,6);
            Rectangle(dc,cx-px(3),cy+px(1),cx+px(4),cy+px(6));MoveToEx(dc,cx-px(3),cy-px(6),nullptr);LineTo(dc,cx-px(3),cy-px(2));LineTo(dc,cx+px(3),cy-px(2));LineTo(dc,cx+px(3),cy-px(6));
        }
    }else if(send){
        int cx=rect.left+px(26);POINT plane[]={{cx-px(7),cy-px(6)},{cx+px(7),cy},{cx-px(7),cy+px(6)},{cx-px(4),cy},{cx-px(7),cy-px(6)}};Polyline(dc,plane,5);MoveToEx(dc,cx-px(4),cy,nullptr);LineTo(dc,cx+px(7),cy);
    }
    SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(pen);SelectObject(dc,oldFont);
    if(save&&gSaveStatus==L"未保存"){RECT dot{saveIconCenter+px(4),cy-px(11),saveIconCenter+px(9),cy-px(6)};fillRoundRect(dc,dot,COLOR_ACCENT,5);}
    if((draw->itemState&ODS_FOCUS)&&!(draw->itemState&ODS_NOFOCUSRECT)){RECT focus=rect;InflateRect(&focus,-px(4),-px(4));DrawFocusRect(dc,&focus);}
}

void drawButton(const DRAWITEMSTRUCT* draw) {
    if(draw->CtlID==IDC_METHOD||draw->CtlID==IDC_URL_FRAME||draw->CtlID==IDC_SAVE||draw->CtlID==IDC_SAVE_MORE||draw->CtlID==IDC_SEND||draw->CtlID==IDC_CANCEL){drawRequestControl(draw);return;}
    if(draw->CtlID==IDC_RESPONSE_MODE_DIVIDER){
        FillRect(draw->hDC,&draw->rcItem,gWhiteBrush);
        RECT line=draw->rcItem;line.left=(line.left+line.right)/2;line.right=line.left+px(1);line.top+=px(6);line.bottom-=px(6);
        FillRect(draw->hDC,&line,gBorderBrush);return;
    }
    if(draw->CtlID==IDC_RESPONSE_MODE||draw->CtlID==IDC_RESPONSE_FIND){
        bool disabled=(draw->itemState&ODS_DISABLED)!=0,pressed=(draw->itemState&ODS_SELECTED)!=0;
        bool hot=!disabled&&gResponseToolbarHot==draw->hwndItem;
        FillRect(draw->hDC,&draw->rcItem,gWhiteBrush);
        if(!disabled&&(hot||pressed))fillRoundRect(draw->hDC,draw->rcItem,pressed?RGB(229,234,241):COLOR_HOVER,6);
        COLORREF foreground=disabled?RGB(156,163,175):(hot||pressed?COLOR_PRIMARY:COLOR_SECONDARY);
        // Retain the window text as the accessible name; the toolbar shows icons only.
        HPEN pen=CreatePen(PS_SOLID,px(1),foreground);auto oldPen=(HPEN)SelectObject(draw->hDC,pen);auto oldBrush=(HBRUSH)SelectObject(draw->hDC,GetStockObject(NULL_BRUSH));
        int cx=(draw->rcItem.left+draw->rcItem.right)/2,cy=(draw->rcItem.top+draw->rcItem.bottom)/2;
        if(draw->CtlID==IDC_RESPONSE_MODE){
            int x=cx-px(5);
            for(int side:{-1,1}){
                POINT brace[]={{x+side*px(2),cy-px(6)},{x+side*px(4),cy-px(6)},{x+side*px(4),cy-px(2)},
                    {x+side*px(6),cy},{x+side*px(4),cy+px(2)},{x+side*px(4),cy+px(6)},{x+side*px(2),cy+px(6)}};
                Polyline(draw->hDC,brace,7);
            }
            int arrow=cx+px(11);MoveToEx(draw->hDC,arrow-px(3),cy-px(1),nullptr);LineTo(draw->hDC,arrow,cy+px(2));LineTo(draw->hDC,arrow+px(3),cy-px(1));
        }else{
            int x=cx-px(6);Ellipse(draw->hDC,x,cy-px(6),x+px(10),cy+px(4));MoveToEx(draw->hDC,x+px(8),cy+px(3),nullptr);LineTo(draw->hDC,x+px(13),cy+px(8));
        }
        SelectObject(draw->hDC,oldBrush);SelectObject(draw->hDC,oldPen);DeleteObject(pen);
        if((draw->itemState&ODS_FOCUS)&&!(draw->itemState&ODS_NOFOCUSRECT)){RECT focus=draw->rcItem;InflateRect(&focus,-px(3),-px(3));DrawFocusRect(draw->hDC,&focus);}return;
    }
    if(draw->CtlID==IDC_RESPONSE_FIND_PANEL){
        HBRUSH background=CreateSolidBrush(RGB(255,255,255));FillRect(draw->hDC,&draw->rcItem,background);DeleteObject(background);
        HPEN pen=CreatePen(PS_SOLID,px(1),RGB(209,213,219));auto oldPen=(HPEN)SelectObject(draw->hDC,pen);auto oldBrush=(HBRUSH)SelectObject(draw->hDC,GetStockObject(NULL_BRUSH));
        Rectangle(draw->hDC,draw->rcItem.left,draw->rcItem.top,draw->rcItem.right,draw->rcItem.bottom);SelectObject(draw->hDC,oldBrush);SelectObject(draw->hDC,oldPen);DeleteObject(pen);return;
    }
    if(draw->CtlID==IDC_RESPONSE_FIND_PREV||draw->CtlID==IDC_RESPONSE_FIND_NEXT||draw->CtlID==IDC_RESPONSE_FIND_CLOSE){
        bool pressed=(draw->itemState&ODS_SELECTED)!=0;bool hot=(draw->itemState&ODS_HOTLIGHT)!=0;
        COLORREF background=pressed?RGB(229,231,235):(hot?RGB(243,244,246):RGB(255,255,255));
        HBRUSH backgroundBrush=CreateSolidBrush(background);FillRect(draw->hDC,&draw->rcItem,backgroundBrush);DeleteObject(backgroundBrush);
        HPEN separator=CreatePen(PS_SOLID,px(1),RGB(229,231,235));auto oldPen=(HPEN)SelectObject(draw->hDC,separator);
        MoveToEx(draw->hDC,draw->rcItem.left,draw->rcItem.top+px(5),nullptr);LineTo(draw->hDC,draw->rcItem.left,draw->rcItem.bottom-px(5));
        SelectObject(draw->hDC,oldPen);DeleteObject(separator);
        int cx=(draw->rcItem.left+draw->rcItem.right)/2,cy=(draw->rcItem.top+draw->rcItem.bottom)/2+(pressed?px(1):0);
        HPEN glyph=CreatePen(PS_SOLID,px(2),(draw->itemState&ODS_DISABLED)?RGB(190,195,203):RGB(75,85,99));oldPen=(HPEN)SelectObject(draw->hDC,glyph);
        if(draw->CtlID==IDC_RESPONSE_FIND_CLOSE){
            int radius=px(4);MoveToEx(draw->hDC,cx-radius,cy-radius,nullptr);LineTo(draw->hDC,cx+radius,cy+radius);
            MoveToEx(draw->hDC,cx+radius,cy-radius,nullptr);LineTo(draw->hDC,cx-radius,cy+radius);
        }else{
            int tip=draw->CtlID==IDC_RESPONSE_FIND_PREV?-px(3):px(3);
            MoveToEx(draw->hDC,cx-px(4),cy-tip,nullptr);LineTo(draw->hDC,cx,cy+tip);LineTo(draw->hDC,cx+px(4),cy-tip);
        }
        SelectObject(draw->hDC,oldPen);DeleteObject(glyph);
        if(draw->itemState&ODS_FOCUS){RECT focus=draw->rcItem;InflateRect(&focus,-px(3),-px(3));DrawFocusRect(draw->hDC,&focus);}return;
    }
    bool primary=draw->CtlID==IDC_SEND;bool pressed=(draw->itemState&ODS_SELECTED)!=0;bool disabled=(draw->itemState&ODS_DISABLED)!=0;
    COLORREF background=primary?(pressed?RGB(29,78,216):COLOR_ACCENT):(pressed?COLOR_HOVER:RGB(255,255,255));
    HBRUSH brush=CreateSolidBrush(background);FillRect(draw->hDC,&draw->rcItem,brush);DeleteObject(brush);
    HPEN pen=CreatePen(PS_SOLID,1,primary?COLOR_ACCENT:COLOR_BORDER);HPEN oldPen=(HPEN)SelectObject(draw->hDC,pen);HBRUSH oldBrush=(HBRUSH)SelectObject(draw->hDC,GetStockObject(NULL_BRUSH));
    Rectangle(draw->hDC,draw->rcItem.left,draw->rcItem.top,draw->rcItem.right,draw->rcItem.bottom);
    SelectObject(draw->hDC,oldBrush);SelectObject(draw->hDC,oldPen);DeleteObject(pen);
    wchar_t text[128]{};GetWindowTextW(draw->hwndItem,text,128);HFONT oldFont=(HFONT)SelectObject(draw->hDC,gUiFont);
    SetBkMode(draw->hDC,TRANSPARENT);SetTextColor(draw->hDC,disabled?RGB(156,163,175):(primary?RGB(255,255,255):COLOR_PRIMARY));
    RECT textRect=draw->rcItem;DrawTextW(draw->hDC,text,-1,&textRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);SelectObject(draw->hDC,oldFont);
    if(draw->itemState&ODS_FOCUS){RECT focus=draw->rcItem;InflateRect(&focus,-px(3),-px(3));DrawFocusRect(draw->hDC,&focus);}
}

ApiFolder* findFolderForRequestIn(ApiFolder& folder,ApiRequest* request) {
    for(auto& item:folder.requests)if(item.get()==request)return &folder;
    for(auto& child:folder.children)if(auto found=findFolderForRequestIn(*child,request))return found;
    return nullptr;
}
ApiFolder* findFolderForRequest(ApiRequest* request){for(auto& f:gData.folders)if(auto found=findFolderForRequestIn(*f,request))return found;return nullptr;}
ApiFolder* findFolderByIdIn(ApiFolder& folder,const string& id) {
    if(folder.id==id)return &folder;for(auto& child:folder.children)if(auto found=findFolderByIdIn(*child,id))return found;return nullptr;
}
ApiFolder* findFolderById(const string& id){for(auto& folder:gData.folders)if(auto found=findFolderByIdIn(*folder,id))return found;return nullptr;}
std::unique_ptr<ApiFolder> cloneFolderForImport(const ApiFolder& source) {
    auto result=std::make_unique<ApiFolder>();result->id=source.id;result->name=source.name;result->expanded=source.expanded;
    for(const auto& child:source.children)result->children.push_back(cloneFolderForImport(*child));
    for(const auto& request:source.requests){auto copy=cloneRequest(*request);copy->id=request->id;result->requests.push_back(std::move(copy));}return result;
}
ApiRequest* firstRequestIn(ApiFolder& folder){if(!folder.requests.empty())return folder.requests.front().get();for(auto& child:folder.children)if(auto request=firstRequestIn(*child))return request;return nullptr;}
bool containsText(const string& value,const wstring& query){wstring hay=toWide(value);return StrStrIW(hay.c_str(),query.c_str())!=nullptr;}

HTREEITEM insertTreeItem(HTREEITEM parent,const wstring& label,NodeRef::Kind kind,void* value,ApiFolder* ownerFolder,ApiRequest* ownerRequest) {
    auto ref=std::make_unique<NodeRef>();ref->kind=kind;ref->value=value;ref->ownerFolder=ownerFolder;ref->ownerRequest=ownerRequest;
    TVINSERTSTRUCTW insert{};insert.hParent=parent;insert.hInsertAfter=TVI_LAST;insert.item.mask=TVIF_TEXT|TVIF_PARAM;
    insert.item.pszText=(LPWSTR)label.c_str();insert.item.lParam=(LPARAM)ref.get();
    HTREEITEM item=TreeView_InsertItem(gTree,&insert);gNodeRefs.push_back(std::move(ref));return item;
}
bool folderMatches(const ApiFolder& folder,const wstring& query) {
    if(query.empty()||containsText(folder.name,query))return true;
    for(auto& r:folder.requests)if(containsText(r->name,query)||containsText(r->url,query))return true;
    for(auto& c:folder.children)if(folderMatches(*c,query))return true;return false;
}
void insertFolder(ApiFolder& folder,HTREEITEM parent,const wstring& query,bool parentMatches=false) {
    bool ownMatch=parentMatches||query.empty()||containsText(folder.name,query);if(!ownMatch&&!folderMatches(folder,query))return;
    wstring label=toWide(folder.name);
    HTREEITEM item=insertTreeItem(parent,label,NodeRef::Kind::Folder,&folder,&folder,nullptr);
    for(auto& child:folder.children)insertFolder(*child,item,query,ownMatch);
    for(auto& request:folder.requests) {
        if(!ownMatch&&!containsText(request->name,query)&&!containsText(request->url,query))continue;
        wstring requestLabel=toWide(request->name);
        HTREEITEM requestItem=insertTreeItem(item,requestLabel,NodeRef::Kind::Request,request.get(),&folder,request.get());
        for(auto& c:request->cases)insertTreeItem(requestItem,toWide(c->name),NodeRef::Kind::Case,c.get(),&folder,request.get());
        if(gExpandedRequests.find(request->id)!=gExpandedRequests.end())TreeView_Expand(gTree,requestItem,TVE_EXPAND);
    }
    if(folder.expanded)TreeView_Expand(gTree,item,TVE_EXPAND);
}
bool treeItemIsRevealed(HTREEITEM item) {
    for(HTREEITEM parent=TreeView_GetParent(gTree,item);parent;parent=TreeView_GetParent(gTree,parent)){
        TVITEMW info{};info.hItem=parent;info.mask=TVIF_STATE;info.stateMask=TVIS_EXPANDED;TreeView_GetItem(gTree,&info);
        if((info.state&TVIS_EXPANDED)==0)return false;
    }
    return true;
}
void syncVisibleFolderExpansionState() {
    if(!gTree)return;
    std::function<void(HTREEITEM)> sync=[&](HTREEITEM parent){
        for(HTREEITEM item=TreeView_GetChild(gTree,parent);item;item=TreeView_GetNextSibling(gTree,item)){
            TVITEMW info{};info.hItem=item;info.mask=TVIF_PARAM|TVIF_STATE;info.stateMask=TVIS_EXPANDED;
            if(TreeView_GetItem(gTree,&info)){
                auto ref=(NodeRef*)info.lParam;
                if(ref&&ref->kind==NodeRef::Kind::Folder)((ApiFolder*)ref->value)->expanded=(info.state&TVIS_EXPANDED)!=0;
            }
            sync(item);
        }
    };
    sync(TVI_ROOT);
}
void rebuildTree() {
    NodeRef::Kind preserveKind=NodeRef::Kind::Folder;void* preserveValue=nullptr;
    if(gTreeSelection){preserveKind=gTreeSelection->kind;preserveValue=gTreeSelection->value;}
    gRebuildingTree=true;
    gTreeSelection=nullptr;TreeView_DeleteAllItems(gTree);gNodeRefs.clear();wstring query=trimWide(textOf(gSearch));
    for(auto& folder:gData.folders)insertFolder(*folder,TVI_ROOT,query,false);
    if(preserveValue){
        std::function<HTREEITEM(HTREEITEM)> find=[&](HTREEITEM parent)->HTREEITEM{
            for(HTREEITEM item=TreeView_GetChild(gTree,parent);item;item=TreeView_GetNextSibling(gTree,item)){
                TVITEMW info{};info.hItem=item;info.mask=TVIF_PARAM;TreeView_GetItem(gTree,&info);auto ref=(NodeRef*)info.lParam;
                if(ref&&ref->kind==preserveKind&&ref->value==preserveValue)return item;
                if(auto found=find(item))return found;
            }return (HTREEITEM)nullptr;
        };
        if(auto found=find(TVI_ROOT);found&&treeItemIsRevealed(found)){TreeView_SelectItem(gTree,found);TreeView_EnsureVisible(gTree,found);}
    }
    gRebuildingTree=false;
}

HTREEITEM findTreeRequest(ApiRequest* request) {
    std::function<HTREEITEM(HTREEITEM)> find=[&](HTREEITEM parent)->HTREEITEM{
        for(HTREEITEM item=TreeView_GetChild(gTree,parent);item;item=TreeView_GetNextSibling(gTree,item)){
            TVITEMW info{};info.hItem=item;info.mask=TVIF_PARAM;TreeView_GetItem(gTree,&info);auto ref=(NodeRef*)info.lParam;
            if(ref&&ref->kind==NodeRef::Kind::Request&&ref->value==request)return item;
            if(auto found=find(item))return found;
        }return (HTREEITEM)nullptr;
    };
    return find(TVI_ROOT);
}
HTREEITEM findTreeValue(NodeRef::Kind kind,void* value) {
    std::function<HTREEITEM(HTREEITEM)> find=[&](HTREEITEM parent)->HTREEITEM{
        for(HTREEITEM item=TreeView_GetChild(gTree,parent);item;item=TreeView_GetNextSibling(gTree,item)){
            TVITEMW info{};info.hItem=item;info.mask=TVIF_PARAM;TreeView_GetItem(gTree,&info);auto ref=(NodeRef*)info.lParam;
            if(ref&&ref->kind==kind&&ref->value==value)return item;if(auto found=find(item))return found;
        }return (HTREEITEM)nullptr;
    };return find(TVI_ROOT);
}
void selectTreeValue(NodeRef::Kind kind,void* value) {
    if(auto item=findTreeValue(kind,value);item&&treeItemIsRevealed(item)){gSelectingTree=true;TreeView_SelectItem(gTree,item);TreeView_EnsureVisible(gTree,item);gSelectingTree=false;}
}
void selectTreeRequest(ApiRequest* request) {
    if(!request)return;if(auto item=findTreeRequest(request);item&&treeItemIsRevealed(item)){gSelectingTree=true;TreeView_SelectItem(gTree,item);TreeView_EnsureVisible(gTree,item);gSelectingTree=false;}
}
void selectTreeTab(const std::shared_ptr<TabState>& tab) {
    if(!tab)return;if(tab->requestCase)selectTreeValue(NodeRef::Kind::Case,tab->requestCase);else selectTreeRequest(tab->request);
}

wstring promptProcResult;bool promptAccepted=false;
LRESULT CALLBACK promptProc(HWND h,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_CREATE) {
        auto create=(CREATESTRUCTW*)l;auto values=(std::vector<wstring>*)create->lpCreateParams;
        CreateWindowW(L"STATIC",(*values)[1].c_str(),WS_CHILD|WS_VISIBLE,px(18),px(18),px(414),px(24),h,nullptr,gInstance,nullptr);
        HWND edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",(*values)[2].c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,px(18),px(48),px(414),px(30),h,(HMENU)IDC_PROMPT_EDIT,gInstance,nullptr);
        CreateWindowW(L"BUTTON",L"确定",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,px(266),px(92),px(80),px(30),h,(HMENU)IDOK,gInstance,nullptr);
        CreateWindowW(L"BUTTON",L"取消",WS_CHILD|WS_VISIBLE,px(352),px(92),px(80),px(30),h,(HMENU)IDCANCEL,gInstance,nullptr);
        for(HWND c=GetWindow(h,GW_CHILD);c;c=GetWindow(c,GW_HWNDNEXT))applyFont(c);SetFocus(edit);SendMessageW(edit,EM_SETSEL,0,-1);return 0;
    }
    if(message==WM_COMMAND&&HIWORD(w)==BN_CLICKED&&(LOWORD(w)==IDOK||LOWORD(w)==IDCANCEL)) {
        if(LOWORD(w)==IDOK){promptProcResult=trimWide(textOf(GetDlgItem(h,IDC_PROMPT_EDIT)));if(promptProcResult.empty()){MessageBoxW(h,L"名称不能为空。",L"FeatherApi",MB_OK|MB_ICONINFORMATION);SetFocus(GetDlgItem(h,IDC_PROMPT_EDIT));return 0;}promptAccepted=true;}DestroyWindow(h);return 0;
    }
    if(message==WM_CLOSE){DestroyWindow(h);return 0;}return DefWindowProcW(h,message,w,l);
}
bool prompt(const wstring& title,const wstring& message,const wstring& initial,wstring& result) {
    static bool registered=false;if(!registered){WNDCLASSW wc{};wc.hInstance=gInstance;wc.lpfnWndProc=promptProc;wc.lpszClassName=L"FeatherApiPrompt";wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);registered=true;}
    std::vector<wstring> values{title,message,initial};RECT owner{};GetWindowRect(gWindow,&owner);
    promptAccepted=false;promptProcResult.clear();HWND dialog=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT,L"FeatherApiPrompt",title.c_str(),WS_CAPTION|WS_SYSMENU,
        owner.left+(owner.right-owner.left-px(466))/2,owner.top+(owner.bottom-owner.top-px(165))/2,px(466),px(165),gWindow,nullptr,gInstance,&values);
    if(!dialog)return false;
    EnableWindow(gWindow,FALSE);ShowWindow(dialog,SW_SHOW);SetForegroundWindow(dialog);MSG msg;
    while(IsWindow(dialog)&&GetMessageW(&msg,nullptr,0,0)>0){if(!IsDialogMessageW(dialog,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    EnableWindow(gWindow,TRUE);SetForegroundWindow(gWindow);if(promptAccepted)result=promptProcResult;return promptAccepted;
}

LRESULT CALLBACK folderPickerProc(HWND h,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_CREATE) {
        auto* context=(FolderPickerContext*)((CREATESTRUCTW*)l)->lpCreateParams;
        SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)context);
        CreateWindowW(L"STATIC",L"选择目标目录",WS_CHILD|WS_VISIBLE,px(18),px(16),px(420),px(24),h,nullptr,gInstance,nullptr);
        HWND combo=CreateWindowExW(WS_EX_CLIENTEDGE,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,px(18),px(45),px(420),px(300),h,(HMENU)1,gInstance,nullptr);
        for(const auto& choice:*context->choices)SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)choice.label.c_str());
        SendMessageW(combo,CB_SETCURSEL,0,0);
        CreateWindowW(L"BUTTON",L"移动",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,px(268),px(92),px(80),px(30),h,(HMENU)IDOK,gInstance,nullptr);
        CreateWindowW(L"BUTTON",L"取消",WS_CHILD|WS_VISIBLE,px(358),px(92),px(80),px(30),h,(HMENU)IDCANCEL,gInstance,nullptr);
        for(HWND child=GetWindow(h,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))applyFont(child);
        SetFocus(combo);return 0;
    }
    if(message==WM_COMMAND&&(LOWORD(w)==IDOK||LOWORD(w)==IDCANCEL)) {
        auto* context=(FolderPickerContext*)GetWindowLongPtrW(h,GWLP_USERDATA);
        if(LOWORD(w)==IDOK&&context&&context->choices){
            int index=(int)SendMessageW(GetDlgItem(h,1),CB_GETCURSEL,0,0);
            if(index<0||index>=(int)context->choices->size()){
                MessageBoxW(h,L"请选择目标目录。",L"FeatherApi",MB_OK|MB_ICONINFORMATION);
                return 0;
            }
            context->selected=(*context->choices)[(size_t)index].folder;context->accepted=true;
        }
        DestroyWindow(h);return 0;
    }
    if(message==WM_CLOSE){DestroyWindow(h);return 0;}
    return DefWindowProcW(h,message,w,l);
}
bool pickFolder(const std::vector<FolderChoice>& choices,ApiFolder*& selected) {
    static bool registered=false;
    if(!registered){WNDCLASSW wc{};wc.hInstance=gInstance;wc.lpfnWndProc=folderPickerProc;wc.lpszClassName=L"FeatherApiFolderPicker";wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);registered=true;}
    FolderPickerContext context{&choices,nullptr,false};RECT owner{};GetWindowRect(gWindow,&owner);
    HWND dialog=CreateWindowExW(WS_EX_DLGMODALFRAME,L"FeatherApiFolderPicker",L"移动接口",WS_CAPTION|WS_SYSMENU,
        owner.left+(owner.right-owner.left-px(470))/2,owner.top+(owner.bottom-owner.top-px(165))/2,px(470),px(165),gWindow,nullptr,gInstance,&context);
    EnableWindow(gWindow,FALSE);ShowWindow(dialog,SW_SHOW);MSG msg;
    while(IsWindow(dialog)&&GetMessageW(&msg,nullptr,0,0)>0){if(!IsDialogMessageW(dialog,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    EnableWindow(gWindow,TRUE);SetForegroundWindow(gWindow);if(context.accepted){selected=context.selected;return true;}return false;
}

void refreshRequestTabs() {
    gRequestTabHot=gRequestTabCloseHot=-1;
    SendMessageW(gRequestTabs,WM_SETREDRAW,FALSE,0);
    TabCtrl_DeleteAllItems(gRequestTabs);
    for(auto& tab:gTabs){auto& shown=tab->caseSnapshot?*tab->caseSnapshot:*tab->request;tab->displayedDirty=tabDirty(*tab);wstring label=toWide(shown.method)+L"  "+toWide(shown.name)+L"     ×";TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=(LPWSTR)label.c_str();TabCtrl_InsertItem(gRequestTabs,TabCtrl_GetItemCount(gRequestTabs),&item);}
    if(gSelectedTab>=0)TabCtrl_SetCurSel(gRequestTabs,gSelectedTab);
    SendMessageW(gRequestTabs,WM_SETREDRAW,TRUE,0);InvalidateRect(gRequestTabs,nullptr,FALSE);
    gEnsureSelectedRequestTabVisible=true;
    PostMessageW(gWindow,WM_UPDATE_TAB_SCROLL,0,0);
}

RECT requestTabScrollThumbRect(HWND scroll);
void positionRequestTabs() {
    if(!gRequestTabs||gRequestTabViewportWidth<=0)return;
    gRequestTabScrollOffset=std::clamp(gRequestTabScrollOffset,0,std::max(0,gRequestTabContentWidth-gRequestTabViewportWidth));
    // Moving and repainting are coordinated by setRequestTabScrollOffset().
    SetWindowPos(gRequestTabs,nullptr,gRequestTabViewportX-gRequestTabScrollOffset,0,gRequestTabContentWidth,px(REQUEST_TAB_HEIGHT),SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW|SWP_NOCOPYBITS);
    HRGN clip=CreateRectRgn(gRequestTabScrollOffset,0,gRequestTabScrollOffset+gRequestTabViewportWidth,px(REQUEST_TAB_HEIGHT));if(!SetWindowRgn(gRequestTabs,clip,FALSE))DeleteObject(clip);
}
void updateRequestTabScroll() {
    if(!gRequestTabs||!gRequestTabScroll)return;
    HDC dc=GetDC(gRequestTabs);HFONT oldFont=dc?(HFONT)SelectObject(dc,gUiFont):nullptr;int contentWidth=0;
    for(int index=0;index<TabCtrl_GetItemCount(gRequestTabs);++index){TCITEMW item{};wchar_t text[512]{};item.mask=TCIF_TEXT;item.pszText=text;item.cchTextMax=512;TabCtrl_GetItem(gRequestTabs,index,&item);SIZE size{};if(dc)GetTextExtentPoint32W(dc,text,(int)wcslen(text),&size);contentWidth+=size.cx+px(48);}
    if(dc){SelectObject(dc,oldFont);ReleaseDC(gRequestTabs,dc);}
    gRequestTabContentWidth=std::max(gRequestTabViewportWidth,contentWidth+px(4));
    positionRequestTabs();
    int count=TabCtrl_GetItemCount(gRequestTabs);if(count>0){RECT last{};if(TabCtrl_GetItemRect(gRequestTabs,count-1,&last)){gRequestTabContentWidth=std::max(gRequestTabViewportWidth,(int)last.right+px(4));positionRequestTabs();}}
    if(HWND upDown=FindWindowExW(gRequestTabs,nullptr,UPDOWN_CLASSW,nullptr))ShowWindow(upDown,SW_HIDE);
    if(gEnsureSelectedRequestTabVisible&&gSelectedTab>=0){
        RECT selected{};TabCtrl_GetItemRect(gRequestTabs,gSelectedTab,&selected);
        if(selected.left<gRequestTabScrollOffset)gRequestTabScrollOffset=selected.left;else if(selected.right>gRequestTabScrollOffset+gRequestTabViewportWidth)gRequestTabScrollOffset=selected.right-gRequestTabViewportWidth;
        gEnsureSelectedRequestTabVisible=false;
    }
    bool overflow=gRequestTabContentWidth>gRequestTabViewportWidth;
    ShowWindow(gRequestTabScroll,overflow?SW_SHOW:SW_HIDE);
    if(!overflow)gRequestTabScrollOffset=0;
    positionRequestTabs();
    InvalidateRect(gRequestTabs,nullptr,FALSE);
    if(!overflow)return;
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS};
    info.nMin=0;info.nMax=std::max(0,gRequestTabContentWidth-1);
    info.nPage=(UINT)gRequestTabViewportWidth;info.nPos=gRequestTabScrollOffset;
    SetScrollInfo(gRequestTabScroll,SB_CTL,&info,TRUE);
}
void setRequestTabScrollOffset(int position) {
    SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(gRequestTabScroll,SB_CTL,&info);
    int maximum=std::max(info.nMin,info.nMax-(int)info.nPage+1);position=std::clamp(position,info.nMin,maximum);
    if(position==gRequestTabScrollOffset)return;
    gRequestTabScrollOffset=position;gRequestTabHot=gRequestTabCloseHot=-1;positionRequestTabs();info.fMask=SIF_POS;info.nPos=position;SetScrollInfo(gRequestTabScroll,SB_CTL,&info,FALSE);
    InvalidateRect(gRequestTabScroll,nullptr,FALSE);UpdateWindow(gRequestTabScroll);
    RECT visible{gRequestTabScrollOffset,0,gRequestTabScrollOffset+gRequestTabViewportWidth,px(REQUEST_TAB_HEIGHT)};
    RedrawWindow(gRequestTabs,&visible,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_NOERASE);
}
void scrollRequestTabs(WPARAM value) {
    SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(gRequestTabScroll,SB_CTL,&info);
    int position=gRequestTabScrollOffset;
    switch(LOWORD(value)){
    case SB_LINELEFT:position-=px(24);break;case SB_LINERIGHT:position+=px(24);break;
    case SB_PAGELEFT:position-=(int)info.nPage;break;case SB_PAGERIGHT:position+=(int)info.nPage;break;
    case SB_THUMBPOSITION:position=(int)HIWORD(value);break;
    case SB_THUMBTRACK:position=info.nTrackPos;break;default:return;
    }
    setRequestTabScrollOffset(position);
}

RECT requestTabScrollThumbRect(HWND scroll) {
    RECT client{};GetClientRect(scroll,&client);
    SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(scroll,SB_CTL,&info);
    int width=std::max(0,(int)client.right-(int)client.left);
    int range=std::max(1,info.nMax-info.nMin+1);
    int minimumThumbWidth=std::min(px(28),width);
    int thumbWidth=std::clamp(MulDiv(width,(int)info.nPage,range),minimumThumbWidth,width);
    int maximum=std::max(info.nMin,info.nMax-(int)info.nPage+1);
    int travel=std::max(0,width-thumbWidth);
    int positionRange=std::max(1,maximum-info.nMin);
    int left=client.left+(maximum==info.nMin?0:MulDiv(info.nPos-info.nMin,travel,positionRange));
    int thumbHeight=std::min(px(5),std::max(1,(int)client.bottom-(int)client.top));
    int top=client.top+((int)client.bottom-(int)client.top-thumbHeight)/2;
    return {left,top,left+thumbWidth,top+thumbHeight};
}

void setRequestTabScrollFromThumb(HWND scroll,int thumbLeft) {
    RECT client{};GetClientRect(scroll,&client);
    SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(scroll,SB_CTL,&info);
    RECT thumb=requestTabScrollThumbRect(scroll);
    int travel=std::max(0,(int)client.right-(int)client.left-((int)thumb.right-(int)thumb.left));
    int maximum=std::max(info.nMin,info.nMax-(int)info.nPage+1);
    int position=travel==0?info.nMin:info.nMin+MulDiv(std::clamp(thumbLeft,(int)client.left,(int)client.left+travel)-(int)client.left,maximum-info.nMin,travel);
    setRequestTabScrollOffset(position);
}

LRESULT CALLBACK requestTabScrollProc(HWND h,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
    switch(message){
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{
        PAINTSTRUCT paint{};HDC dc=BeginPaint(h,&paint);RECT client{};GetClientRect(h,&client);
        int width=client.right-client.left,height=client.bottom-client.top;
        if(width>0&&height>0){
            HDC buffer=CreateCompatibleDC(dc);HBITMAP bitmap=CreateCompatibleBitmap(dc,width,height);HGDIOBJ oldBitmap=SelectObject(buffer,bitmap);
            FillRect(buffer,&client,gSidebarBrush);RECT thumb=requestTabScrollThumbRect(h);fillRoundRect(buffer,thumb,RGB(156,163,175),px(4));
            BitBlt(dc,paint.rcPaint.left,paint.rcPaint.top,paint.rcPaint.right-paint.rcPaint.left,paint.rcPaint.bottom-paint.rcPaint.top,buffer,paint.rcPaint.left,paint.rcPaint.top,SRCCOPY);
            SelectObject(buffer,oldBitmap);DeleteObject(bitmap);DeleteDC(buffer);
        }
        EndPaint(h,&paint);return 0;
    }
    case WM_LBUTTONDOWN:{
        SetFocus(gRequestTabs);RECT thumb=requestTabScrollThumbRect(h);int x=(short)LOWORD(l);
        if(x>=thumb.left&&x<thumb.right){gDraggingRequestTabScroll=true;gRequestTabScrollDragOffset=x-thumb.left;SetCapture(h);}
        else setRequestTabScrollFromThumb(h,x-(thumb.right-thumb.left)/2);
        return 0;
    }
    case WM_MOUSEMOVE:
        if(gDraggingRequestTabScroll&&(w&MK_LBUTTON)){setRequestTabScrollFromThumb(h,(short)LOWORD(l)-gRequestTabScrollDragOffset);return 0;}
        break;
    case WM_LBUTTONUP:
        if(gDraggingRequestTabScroll){gDraggingRequestTabScroll=false;ReleaseCapture();RedrawWindow(gRequestTabs,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_NOERASE);return 0;}
        break;
    case WM_CAPTURECHANGED:
        if(gDraggingRequestTabScroll){gDraggingRequestTabScroll=false;RedrawWindow(gRequestTabs,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_NOERASE);}
        break;
    }
    return DefSubclassProc(h,message,w,l);
}

LRESULT CALLBACK requestTabsProc(HWND h,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR) {
    if(message==WM_ERASEBKGND)return 1;
    // Native tab controls do not send NM_CUSTOMDRAW. Keep their semantics and
    // geometry, but render both the screen and print path with our own strip.
    if(message==WM_PRINTCLIENT){paintRequestTabs(h,(HDC)w);return 0;}
    if(message==WM_PAINT){
        PAINTSTRUCT paint{};HDC dc=BeginPaint(h,&paint);RECT client{};GetClientRect(h,&client);
        RECT dirty=paint.rcPaint;int width=dirty.right-dirty.left,height=dirty.bottom-dirty.top;
        if(width>0&&height>0){
            HDC buffer=CreateCompatibleDC(dc);HBITMAP bitmap=CreateCompatibleBitmap(dc,width,height);HGDIOBJ oldBitmap=SelectObject(buffer,bitmap);
            SetViewportOrgEx(buffer,-dirty.left,-dirty.top,nullptr);IntersectClipRect(buffer,dirty.left,dirty.top,dirty.right,dirty.bottom);
            paintRequestTabs(h,buffer);
            BitBlt(dc,dirty.left,dirty.top,width,height,buffer,dirty.left,dirty.top,SRCCOPY);
            SelectObject(buffer,oldBitmap);DeleteObject(bitmap);DeleteDC(buffer);
        }
        EndPaint(h,&paint);return 0;
    }
    if(message==WM_MOUSEMOVE){
        TCHITTESTINFO hit{};hit.pt={(short)LOWORD(l),(short)HIWORD(l)};int hot=TabCtrl_HitTest(h,&hit),closeHot=-1;
        if(hot>=0){RECT close=requestTabCloseRect(h,hot);if(PtInRect(&close,hit.pt))closeHot=hot;}
        if(hot!=gRequestTabHot||closeHot!=gRequestTabCloseHot){gRequestTabHot=hot;gRequestTabCloseHot=closeHot;InvalidateRect(h,nullptr,FALSE);}
        TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,h,0};TrackMouseEvent(&tracking);
    }
    if(message==WM_MOUSELEAVE||message==WM_ENABLE){gRequestTabHot=gRequestTabCloseHot=-1;InvalidateRect(h,nullptr,FALSE);}
    if(message==WM_LBUTTONDOWN){
        TCHITTESTINFO hit{};hit.pt={(short)LOWORD(l),(short)HIWORD(l)};int index=TabCtrl_HitTest(h,&hit);
        if(index>=0){RECT close=requestTabCloseRect(h,index);if(PtInRect(&close,hit.pt)){closeTab(index);return 0;}}
    }
    if(message==WM_CONTEXTMENU){
        POINT screen{(short)LOWORD(l),(short)HIWORD(l)},client=screen;
        int index=gSelectedTab;
        if(screen.x==-1&&screen.y==-1){
            if(index<0)return 0;RECT tab{};TabCtrl_GetItemRect(h,index,&tab);client={tab.left,tab.bottom};screen=client;ClientToScreen(h,&screen);
        }else{
            ScreenToClient(h,&client);TCHITTESTINFO hit{};hit.pt=client;index=TabCtrl_HitTest(h,&hit);if(index<0)return 0;
        }
        HMENU menu=CreatePopupMenu();int count=(int)gTabs.size();
        AppendMenuW(menu,MF_STRING,IDM_TAB_CLOSE_ALL,L"关闭全部");
        AppendMenuW(menu,count>1?MF_STRING:MF_GRAYED,IDM_TAB_CLOSE_OTHERS,L"关闭其他");
        AppendMenuW(menu,index>0?MF_STRING:MF_GRAYED,IDM_TAB_CLOSE_LEFT,L"关闭左侧");
        AppendMenuW(menu,index+1<count?MF_STRING:MF_GRAYED,IDM_TAB_CLOSE_RIGHT,L"关闭右侧");
        int command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,screen.x,screen.y,0,gWindow,nullptr);DestroyMenu(menu);
        if(command)closeTabRange(index,command);return 0;
    }
    if(message==WM_NCDESTROY){gRequestTabHot=gRequestTabCloseHot=-1;RemoveWindowSubclass(h,requestTabsProc,id);}
    LRESULT result=DefSubclassProc(h,message,w,l);
    if(message==WM_SETFOCUS||message==WM_KILLFOCUS||message==WM_UPDATEUISTATE||message==TCM_SETCURSEL||message==TCM_SETCURFOCUS||message==WM_KEYDOWN||message==WM_LBUTTONDOWN)
        InvalidateRect(h,nullptr,FALSE);
    return result;
}

void paintEntryHeader(HWND header,HDC dc) {
    RECT client{};GetClientRect(header,&client);FillRect(dc,&client,gSidebarBrush);
    int saved=SaveDC(dc);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,COLOR_SECONDARY);SelectObject(dc,gUiFont);
    for(int column=1;column<=4;++column){
        RECT cell{};Header_GetItemRect(header,column,&cell);cell.left+=px(12);cell.right-=px(12);
        wchar_t text[32]{};HDITEMW item{};item.mask=HDI_TEXT;item.pszText=text;item.cchTextMax=(int)std::size(text);Header_GetItem(header,column,&item);
        DrawTextW(dc,text,-1,&cell,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    }
    RECT line{client.left,client.bottom-1,client.right,client.bottom};FillRect(dc,&line,gBorderBrush);RestoreDC(dc,saved);
}
LRESULT CALLBACK entryHeaderProc(HWND h,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR) {
    if(message==HDM_LAYOUT){
        LRESULT result=DefSubclassProc(h,message,w,l);auto layoutInfo=(HDLAYOUT*)l;
        layoutInfo->prc->top+=px(ENTRY_HEADER_HEIGHT)-layoutInfo->pwpos->cy;layoutInfo->pwpos->cy=px(ENTRY_HEADER_HEIGHT);return result;
    }
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT){PAINTSTRUCT paint{};HDC dc=BeginPaint(h,&paint);paintEntryHeader(h,dc);EndPaint(h,&paint);return 0;}
    if(message==WM_PRINTCLIENT){paintEntryHeader(h,(HDC)w);return 0;}
    if(message==WM_NCDESTROY)RemoveWindowSubclass(h,entryHeaderProc,id);
    return DefSubclassProc(h,message,w,l);
}
void updateEntryMetrics() {
    // A small image list sets the native row height, so painting, scrolling and
    // hit testing share the same geometry. The list view owns the final list.
    HIMAGELIST spacing=ImageList_Create(1,px(ENTRY_ROW_HEIGHT)-1,ILC_COLOR32,1,1);
    if(spacing){HIMAGELIST old=ListView_SetImageList(gKvList,spacing,LVSIL_SMALL);if(old)ImageList_Destroy(old);}
    applyFont(ListView_GetHeader(gKvList));
    SetWindowPos(gKvList,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
}
void addListColumns(HWND list,bool enabled) {
    ListView_DeleteAllItems(list);while(ListView_DeleteColumn(list,0));
    auto insertColumn=[&](int index,const wchar_t* text,int width,int format=LVCFMT_LEFT){
        LVCOLUMNW column{};column.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM|LVCF_FMT;column.iSubItem=index;column.pszText=(LPWSTR)text;column.cx=width;column.fmt=format;ListView_InsertColumn(list,index,&column);
    };
    if(enabled){insertColumn(0,L"",px(38),LVCFMT_CENTER);insertColumn(1,L"名称",px(170));insertColumn(2,L"值",px(220));insertColumn(3,L"类型",px(130));insertColumn(4,L"说明",px(260));insertColumn(5,L"",px(42),LVCFMT_CENTER);int order[]{0,1,2,3,4,5};ListView_SetColumnOrderArray(list,6,order);}
    else {insertColumn(0,L"Key",px(210));insertColumn(1,L"Value",px(420));int order[]{0,1};ListView_SetColumnOrderArray(list,2,order);}
}
void resizeEntryColumns(HWND list,bool enabled) {
    RECT client{};GetClientRect(list,&client);int contentWidth=std::max(px(120),(int)client.right-px(2));
    if(enabled){int checkWidth=px(40),deleteWidth=px(36),typeWidth=px(100),available=std::max(px(300),contentWidth-checkWidth-deleteWidth-typeWidth);int keyWidth=available*36/100,valueWidth=available*31/100;ListView_SetColumnWidth(list,0,checkWidth);ListView_SetColumnWidth(list,1,keyWidth);ListView_SetColumnWidth(list,2,valueWidth);ListView_SetColumnWidth(list,3,typeWidth);ListView_SetColumnWidth(list,4,available-keyWidth-valueWidth);ListView_SetColumnWidth(list,5,deleteWidth);}
    else {int available=std::max(px(90),contentWidth);ListView_SetColumnWidth(list,0,available/2);ListView_SetColumnWidth(list,1,available-available/2);}
}
void fillEntryList(HWND list,const std::vector<KeyValueEntry>& entries,bool enabled=true) {
    bool previousLoading=gLoadingEntryList;gLoadingEntryList=true;
    if(list==gKvList)gEntryHotRow=gEntryHotColumn=-1;
    ListView_DeleteAllItems(list);
    int row=0;
    for(const auto& entry:entries){if(enabled&&entry.key.empty()&&entry.value.empty()&&entry.type.empty()&&entry.description.empty())continue;wstring key=toWide(entry.key),value=toWide(entry.value),type=toWide(entry.type),description=toWide(entry.description);LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=row;item.pszText=(LPWSTR)(enabled?L"":key.c_str());ListView_InsertItem(list,&item);
        int offset=enabled?1:0;if(enabled){ListView_SetCheckState(list,row,entry.enabled);ListView_SetItemText(list,row,offset,(LPWSTR)key.c_str());}ListView_SetItemText(list,row,offset+1,(LPWSTR)value.c_str());if(enabled){ListView_SetItemText(list,row,3,(LPWSTR)type.c_str());ListView_SetItemText(list,row,4,(LPWSTR)description.c_str());}++row;}
    if(enabled){LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=row;item.pszText=(LPWSTR)L"";ListView_InsertItem(list,&item);ListView_SetCheckState(list,row,TRUE);}
    gLoadingEntryList=previousLoading;
}
std::vector<KeyValueEntry> readEntryList(HWND list,bool enabled=true) {
    std::vector<KeyValueEntry> result;int count=ListView_GetItemCount(list);
    for(int i=0;i<count;++i){KeyValueEntry entry;entry.enabled=true;int offset=enabled?1:0;if(enabled)entry.enabled=ListView_GetCheckState(list,i)!=FALSE;
        entry.key=toUtf8(listCellText(list,i,offset));entry.value=toUtf8(listCellText(list,i,offset+1));if(enabled){entry.type=toUtf8(listCellText(list,i,3));entry.description=toUtf8(listCellText(list,i,4));}if(!enabled||!entry.key.empty()||!entry.value.empty()||!entry.type.empty()||!entry.description.empty())result.push_back(std::move(entry));}
    return result;
}

void saveEditor(bool updateBodyType=true) {
    if(gLoadingEditor)return;auto tab=selectedTab();if(!tab)return;auto& r=tab->caseSnapshot?*tab->caseSnapshot:*tab->request;
    string previousMethod=r.method;wstring method=trimWide(textOf(gMethod));if(!method.empty())r.method=toUtf8(method);
    r.url=toUtf8(textOf(gUrl));if(updateBodyType){int bodyType=(int)SendMessageW(gBodyType,CB_GETCURSEL,0,0);const char* types[]={"None","JSON","Form URL Encoded","Multipart Form Data","Raw"};if(bodyType>=0&&bodyType<5)r.bodyType=types[bodyType];}r.body=toUtf8(textOf(gBody));
    if(gEditorPage==0)r.query=readEntryList(gKvList);else if(gEditorPage==1)r.headers=readEntryList(gKvList);else if(gEditorPage==2&&(r.bodyType=="Form URL Encoded"||r.bodyType=="Multipart Form Data"))r.formFields=readEntryList(gKvList);
    if(previousMethod!=r.method)refreshRequestTabs();
}
bool saveNow(bool feedback=false) {
    if(gCellEditor)commitCellEditor();saveEditor();syncVisibleFolderExpansionState();
    for(auto& tab:gTabs)if(tab->requestCase)caseContent(*tab).apply(*tab->requestCase);
    if(gServices.saveData(gData)){
        gSavedContent=WorkspaceContent::from(gData);gSaveFailed=false;updateDirtyStatus();
        if(auto tab=selectedTab();tab&&tab->validation==L"保存失败：无法写入本地数据文件。") {tab->validation.clear();setValidationText(L"");}
        rebuildTree();if(feedback)showSaveFeedback(true);else setText(gSave,L"保存");return true;
    }
    gSaveFailed=true;updateDirtyStatus();showSaveFeedback(false);
    if(auto tab=selectedTab()){tab->validation=L"保存失败：无法写入本地数据文件。";setValidationText(tab->validation);}return false;
}
void scheduleSave() {
    SetTimer(gWindow,DIRTY_TIMER,120,nullptr);
}
void markCatalogUnsaved(){setSaveStatus(L"未保存");}
void showEditorPage() {
    auto tab=selectedTab();if(!tab)return;auto& r=tab->caseSnapshot?*tab->caseSnapshot:*tab->request;bool formBody=r.bodyType=="Form URL Encoded"||r.bodyType=="Multipart Form Data";bool listPage=gEditorPage<2||(gEditorPage==2&&formBody);
    if(!listPage&&gCellEditor)commitCellEditor();
    setVisible(gKvList,listPage);
    bool bodyText=gEditorPage==2&&r.bodyType!="None"&&!formBody;
    bool jsonBody=gEditorPage==2&&r.bodyType=="JSON";setVisible(gBodyType,gEditorPage==2);setVisible(gFormat,jsonBody);setVisible(gCompress,jsonBody);setVisible(gBody,bodyText);setVisible(gBodyNone,gEditorPage==2&&r.bodyType=="None");
    if(gEditorPage==0)fillEntryList(gKvList,r.query);else if(gEditorPage==1)fillEntryList(gKvList,r.headers);else if(formBody)fillEntryList(gKvList,r.formFields);
    if(listPage)ListView_SetItemState(gKvList,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
    RECT client{};GetClientRect(gWindow,&client);layout((int)client.right,(int)client.bottom);
}
void captureView() {
    auto tab=gDisplayedTab.lock();if(!tab)return;
    tab->editorPage=gEditorPage;tab->responsePage=gResponsePage;
    tab->findVisible=gResponseFindVisible;tab->findQuery=gResponseFindQuery;tab->findPosition=gResponseFindPosition;
    tab->findStatus=textOf(gFindStatus);
    SendMessageW(gResponseBody,EM_GETSEL,(WPARAM)&tab->selectionStart,(LPARAM)&tab->selectionEnd);
    tab->firstVisibleLine=(int)SendMessageW(gResponseBody,EM_GETFIRSTVISIBLELINE,0,0);
    tab->horizontalScroll=GetScrollPos(gResponseBody,SB_HORZ);
    if(gRichEditModule)SendMessageW(gResponseBody,EM_GETSCROLLPOS,0,(LPARAM)&tab->responseScroll);
}
// Rich Edit character positions use a single CR for each paragraph. Read that
// same representation for coloring, hit testing and search selection offsets.
wstring responseText() {
    if(!gRichEditModule)return textOf(gResponseBody);
    GETTEXTLENGTHEX length{GTL_NUMCHARS|GTL_PRECISE,1200};
    auto count=(size_t)SendMessageW(gResponseBody,EM_GETTEXTLENGTHEX,(WPARAM)&length,0);
    wstring value(count+1,L'\0');GETTEXTEX get{};get.cb=(DWORD)(value.size()*sizeof(wchar_t));get.codepage=1200;
    auto copied=SendMessageW(gResponseBody,EM_GETTEXTEX,(WPARAM)&get,(LPARAM)value.data());
    value.resize((size_t)copied);return value;
}
void updateResponseInsets() {
    RECT rect{};GetClientRect(gResponseBody,&rect);
    rect.left+=px(12);rect.top+=px(9);rect.right=std::max(rect.left+1,rect.right-px(12));rect.bottom=std::max(rect.top+1,rect.bottom-px(9));
    SendMessageW(gResponseBody,EM_SETRECTNP,0,(LPARAM)&rect);
}
void displayResponse(const wstring& value,bool pretty) {
    gResponseHighlights.clear();gResponseHighlightLength=0;gResponseHighlightCurrent=wstring::npos;
    SendMessageW(gResponseBody,WM_SETREDRAW,FALSE,0);setText(gResponseBody,value);
    if(gRichEditModule){
        CHARFORMAT2W format{};format.cbSize=sizeof(format);format.dwMask=CFM_COLOR|CFM_BOLD|CFM_BACKCOLOR;format.crTextColor=RGB(51,65,85);format.dwEffects=CFE_AUTOBACKCOLOR;
        SendMessageW(gResponseBody,EM_SETCHARFORMAT,SCF_ALL,(LPARAM)&format);
        format.dwMask=CFM_COLOR|CFM_BOLD;format.dwEffects=0;
        PARAFORMAT2 paragraph{};paragraph.cbSize=sizeof(paragraph);paragraph.dwMask=PFM_LINESPACING;paragraph.bLineSpacingRule=5;paragraph.dyLineSpacing=24;
        SendMessageW(gResponseBody,EM_SETSEL,0,-1);SendMessageW(gResponseBody,EM_SETPARAFORMAT,0,(LPARAM)&paragraph);
        // Bound per-token native messages for very large responses.
        if(pretty&&value.size()<=512*1024){
            wstring text=responseText();size_t first=text.find_first_not_of(L" \t\r\n");
            if(first!=wstring::npos&&(text[first]==L'{'||text[first]==L'[')){
                size_t spans=0;
                for(size_t i=0;i<text.size()&&spans<20000;){
                    size_t start=i;COLORREF color=RGB(100,116,139);bool bold=false;
                    if(text[i]==L'"'){
                        ++i;while(i<text.size()){if(text[i]==L'\\'){i=std::min(text.size(),i+2);continue;}if(text[i++]==L'"')break;}
                        size_t next=text.find_first_not_of(L" \t\r\n",i);bold=next!=wstring::npos&&text[next]==L':';color=bold?RGB(29,78,160):RGB(21,128,61);
                    }else if(text[i]==L'-'||(text[i]>=L'0'&&text[i]<=L'9')){
                        ++i;while(i<text.size()&&((text[i]>=L'0'&&text[i]<=L'9')||text[i]==L'.'||text[i]==L'e'||text[i]==L'E'||text[i]==L'+'||text[i]==L'-'))++i;color=RGB(180,83,9);
                    }else if(text.compare(i,4,L"true")==0||text.compare(i,4,L"null")==0){i+=4;color=RGB(126,34,206);}
                    else if(text.compare(i,5,L"false")==0){i+=5;color=RGB(126,34,206);}
                    else {++i;continue;}
                    format.crTextColor=color;format.dwEffects=bold?CFE_BOLD:0;
                    SendMessageW(gResponseBody,EM_SETSEL,(WPARAM)start,(LPARAM)i);SendMessageW(gResponseBody,EM_SETCHARFORMAT,SCF_SELECTION,(LPARAM)&format);++spans;
                }
            }
        }
        SendMessageW(gResponseBody,EM_SETSEL,0,0);SendMessageW(gResponseBody,EM_EMPTYUNDOBUFFER,0,0);
    }
    gResponseSearchText=responseText();for(auto& c:gResponseSearchText)c=static_cast<wchar_t>(towlower(c));
    updateResponseInsets();SendMessageW(gResponseBody,WM_SETREDRAW,TRUE,0);InvalidateRect(gResponseBody,nullptr,TRUE);
}
void showResponsePage() {
    auto tab=selectedTab();if(!tab)return;
    setVisible(gResponseBody,gResponsePage==0);setVisible(gResponseHeaders,gResponsePage==1);
    setVisible(gResponseMode,gResponsePage==0);setVisible(gResponseModeDivider,gResponsePage==0);setVisible(gResponseFind,gResponsePage==0&&!gResponseFindVisible);
    bool showFind=gResponsePage==0&&gResponseFindVisible;
    setVisible(gResponseFindPanel,showFind);setVisible(gFindStatus,showFind);
    if(gResponsePage==0){
        displayResponse(toWide(tab->rawView?tab->responseRaw:tab->responsePretty),!tab->rawView);
        setText(gResponseMode,tab->rawView?L"原始":L"格式化");
        TOOLINFOW modeTip{sizeof(modeTip)};modeTip.uFlags=TTF_IDISHWND|TTF_SUBCLASS;modeTip.hwnd=gWindow;modeTip.uId=(UINT_PTR)gResponseMode;
        modeTip.lpszText=(LPWSTR)(tab->rawView?L"响应显示方式：原始（点击切换）":L"响应显示方式：格式化（点击切换）");
        SendMessageW(gSaveTooltip,TTM_UPDATETIPTEXTW,0,(LPARAM)&modeTip);
        SendMessageW(gResponseBody,EM_SETSEL,tab->selectionStart,tab->selectionEnd);
        if(gRichEditModule)SendMessageW(gResponseBody,EM_SETSCROLLPOS,0,(LPARAM)&tab->responseScroll);
        else {
            SendMessageW(gResponseBody,EM_LINESCROLL,0,tab->firstVisibleLine);
            SendMessageW(gResponseBody,WM_HSCROLL,MAKEWPARAM(SB_THUMBPOSITION,tab->horizontalScroll),0);
        }
    }else fillEntryList(gResponseHeaders,tab->responseHeaders,false);
    bool hasResponse=tab->responseSummary!=L"暂无响应";
    EnableWindow(gResponseMode,hasResponse);
    setText(gSummary,tab->summary+(tab->oldResponse?L"   · 正在显示上次响应":L""));InvalidateRect(gSummary,nullptr,TRUE);
    RECT client{};GetClientRect(gWindow,&client);layout((int)client.right,(int)client.bottom);
    if(gResponsePage==0){
        refreshResponseFind(false);
        if(gRichEditModule)SendMessageW(gResponseBody,EM_HIDESELECTION,gResponseFindVisible&&GetFocus()!=gResponseBody,0);
    }
}
void loadEditor() {
    captureView();gLoadingEditor=true;auto tab=selectedTab();bool active=tab!=nullptr;
    for(HWND control:{gRequestTabs,gMethod,gUrlFrame,gUrl,gSave,gSaveMore,gSend,gCancel,gEditorTabs,gSummary,gResponseTabs})EnableWindow(control,active);
    setVisible(gEmptyTitle,!active);setVisible(gEmptyHelp,!active);
    for(HWND control:{gMethod,gUrlFrame,gUrl,gSaveMore,gEditorTabs,gKvList,gBodyType,gFormat,gCompress,gBody,gValidation,gSummary,gResponseTabs,gResponseBody,gResponseHeaders,gBodyNone,gResponseMode,gResponseModeDivider,gResponseFind})setVisible(control,active);
    EnableWindow(gSave,TRUE);setVisible(gSave,true);
    if(!active){gDisplayedTab.reset();setVisible(gResponseFindPanel,false);setVisible(gFindStatus,false);setVisible(gSend,false);setVisible(gCancel,false);setText(gUrl,L"");setText(gResponseBody,L"");gLoadingEditor=false;return;}
    gEditorPage=tab->editorPage;gResponsePage=tab->responsePage;gResponseFindVisible=tab->findVisible;gResponseFindQuery=tab->findQuery;gResponseFindPosition=tab->findPosition;
    TabCtrl_SetCurSel(gEditorTabs,gEditorPage);TabCtrl_SetCurSel(gResponseTabs,gResponsePage);setText(gResponseFindEdit,tab->findQuery);setText(gFindStatus,tab->findStatus);
    auto& r=tab->caseSnapshot?*tab->caseSnapshot:*tab->request;setText(gMethod,toWide(r.method));InvalidateRect(gMethod,nullptr,TRUE);setText(gUrl,toWide(r.url));
    int type=0;const char* types[]={"None","JSON","Form URL Encoded","Multipart Form Data","Raw"};for(int i=0;i<5;++i)if(r.bodyType==types[i])type=i;SendMessageW(gBodyType,CB_SETCURSEL,type,0);setText(gBody,toWide(r.body));
    setText(gSummary,tab->summary);setValidationText(tab->validation);setVisible(gSend,!tab->sending);setVisible(gCancel,tab->sending);
    showEditorPage();showResponsePage();gDisplayedTab=tab;gLoadingEditor=false;
    setText(gCancel,tab->cancel?L"正在取消":L"取消");EnableWindow(gCancel,!tab->cancel);
}
void openRequest(ApiRequest* request,ApiRequestCase* requestCase=nullptr) {
    commitCellEditor();saveEditor();captureView();for(size_t i=0;i<gTabs.size();++i)if(gTabs[i]->request==request&&gTabs[i]->requestCase==requestCase){if(gSelectedTab!=(int)i)for(int& row:gSelectedEntryRows)row=-1;gSelectedTab=(int)i;refreshRequestTabs();loadEditor();selectTreeTab(gTabs[i]);return;}
    auto tab=std::make_shared<TabState>();tab->request=request;tab->requestCase=requestCase;
    if(requestCase){
        tab->caseSnapshot=std::make_unique<ApiRequest>();tab->caseSnapshot->id=request->id;tab->caseSnapshot->name=requestCase->name;tab->caseSnapshot->method=requestCase->method;tab->caseSnapshot->url=requestCase->url;
        tab->caseSnapshot->query=requestCase->query;tab->caseSnapshot->headers=requestCase->headers;tab->caseSnapshot->bodyType=requestCase->bodyType;tab->caseSnapshot->body=requestCase->body;tab->caseSnapshot->formFields=requestCase->formFields;
        tab->responseRaw=requestCase->responseRaw;tab->responsePretty=requestCase->responsePretty;tab->responseHeaders=requestCase->responseHeaders;tab->summary=toWide(requestCase->responseSummary);tab->responseSummary=tab->summary;tab->statusCode=(unsigned)wcstoul(tab->summary.c_str(),nullptr,10);
    }
    gTabs.push_back(tab);for(int& row:gSelectedEntryRows)row=-1;gSelectedTab=(int)gTabs.size()-1;gSelectedFolder=findFolderForRequest(request);refreshRequestTabs();loadEditor();selectTreeTab(tab);
}
// Delete operations already have their own explicit confirmation.
void closeTab(int index,bool confirm) {
    if(index<0||index>=(int)gTabs.size())return;
    if(confirm){closeTabRange(index,0);return;}
    commitCellEditor();saveEditor();captureView();
    auto tab=gTabs[(size_t)index];tab->cancelNow();
    gSelectedTab=tabSelectionAfterClose(gSelectedTab,index,(int)gTabs.size());gTabs.erase(gTabs.begin()+index);
    refreshRequestTabs();loadEditor();selectTreeTab(selectedTab());
}
void closeTabRange(int contextIndex,int command) {
    if(contextIndex<0||contextIndex>=(int)gTabs.size())return;
    commitCellEditor();saveEditor();captureView();
    auto previous=selectedTab();std::vector<std::shared_ptr<TabState>> targets;
    for(int i=0;i<(int)gTabs.size();++i){
        bool target=command==0?i==contextIndex:command==IDM_TAB_CLOSE_ALL?true:
            command==IDM_TAB_CLOSE_OTHERS?i!=contextIndex:command==IDM_TAB_CLOSE_LEFT?i<contextIndex:i>contextIndex;
        if(target)targets.push_back(gTabs[(size_t)i]);
    }
    // Discarding a newly created parent also removes its cases; include their tabs in the one prompt.
    auto initial=targets;
    for(const auto& target:initial)if(!target->requestCase&&!gSavedContent.requests.count(target->request->id))
        for(const auto& tab:gTabs)if(tab->request==target->request&&std::find(targets.begin(),targets.end(),tab)==targets.end())targets.push_back(tab);
    size_t dirty=0;for(const auto& tab:targets)if(tabDirty(*tab))++dirty;
    int choice=IDNO;
    if(dirty){
        choice=askSaveChanges(L"关闭前处理未保存修改",std::to_wstring(dirty)+L" 个目标标签有未保存修改。保存全部会同时保存其他标签和目录修改。\n放弃只恢复目标内容；未保存的新建接口及其用例会被移除。");
        if(choice==IDCANCEL)return;
        if(choice==IDYES&&!saveNow(true))return;
    }
    std::unordered_set<ApiRequest*> removeRequests;
    if(choice==IDNO){
        for(const auto& tab:targets){
            if(tab->requestCase){
                auto found=gSavedContent.cases.find(tab->requestCase->id);
                if(found!=gSavedContent.cases.end())found->second.apply(*tab->requestCase);
                else {auto& cases=tab->request->cases;cases.erase(std::remove_if(cases.begin(),cases.end(),[&](const auto& c){return c.get()==tab->requestCase;}),cases.end());}
            }else{
                auto found=gSavedContent.requests.find(tab->request->id);
                if(found!=gSavedContent.requests.end())found->second.apply(*tab->request);
                else removeRequests.insert(tab->request);
            }
        }
    }
    for(const auto& tab:targets)tab->cancelNow();
    // Do not reload the discarded editor: its text must never overwrite the restored baseline.
    gTabs.erase(std::remove_if(gTabs.begin(),gTabs.end(),[&](const auto& tab){return std::find(targets.begin(),targets.end(),tab)!=targets.end();}),gTabs.end());
    for(auto request:removeRequests)if(auto folder=findFolderForRequest(request)){
        auto& requests=folder->requests;requests.erase(std::remove_if(requests.begin(),requests.end(),[&](const auto& r){return r.get()==request;}),requests.end());
    }
    auto found=std::find(gTabs.begin(),gTabs.end(),previous);
    gSelectedTab=found!=gTabs.end()?(int)std::distance(gTabs.begin(),found):gTabs.empty()?-1:std::min(contextIndex,(int)gTabs.size()-1);
    gDisplayedTab.reset();for(int& row:gSelectedEntryRows)row=-1;
    rebuildTree();refreshRequestTabs();loadEditor();selectTreeTab(selectedTab());updateDirtyStatus();
}

void addFolder(ApiFolder* parent) {
    wstring name;if(!prompt(parent?L"新增子目录":L"新增目录",L"请输入目录名称：",L"新目录",name))return;
    auto folder=std::make_unique<ApiFolder>();folder->id=newId();folder->name=toUtf8(name);ApiFolder* pointer=folder.get();if(parent)parent->children.push_back(std::move(folder));else gData.folders.push_back(std::move(folder));gSelectedFolder=pointer;rebuildTree();selectTreeValue(NodeRef::Kind::Folder,pointer);setSaveStatus(L"未保存");
}
void addRequest(ApiFolder* folder) {
    if(!folder){if(gData.folders.empty()){auto f=std::make_unique<ApiFolder>();f->id=newId();f->name="默认目录";gData.folders.push_back(std::move(f));}folder=gData.folders.front().get();}
    wstring name;if(!prompt(L"新增接口",L"请输入接口名称：",L"新接口",name))return;
    auto request=std::make_unique<ApiRequest>();request->id=newId();request->name=toUtf8(name);ApiRequest* pointer=request.get();folder->requests.push_back(std::move(request));rebuildTree();openRequest(pointer);setSaveStatus(L"未保存");
}
void renameFolder(ApiFolder* folder) {
    if(!folder)return;wstring result;
    if(!prompt(L"重命名目录",L"请输入新的目录名称：",toWide(folder->name),result))return;
    folder->name=toUtf8(result);gSelectedFolder=folder;rebuildTree();selectTreeValue(NodeRef::Kind::Folder,folder);setSaveStatus(L"未保存");
}
void renameRequest(ApiRequest* request) {
    if(!request)return;wstring result;
    if(!prompt(L"重命名接口",L"请输入新的接口名称：",toWide(request->name),result))return;
    request->name=toUtf8(result);rebuildTree();selectTreeValue(NodeRef::Kind::Request,request);refreshRequestTabs();setSaveStatus(L"未保存");
}
void renameSelection() {
    if(!gTreeSelection)return;
    NodeRef::Kind kind=gTreeSelection->kind;void* target=gTreeSelection->value;
    if(kind==NodeRef::Kind::Folder)renameFolder((ApiFolder*)target);
    else if(kind==NodeRef::Kind::Request)renameRequest((ApiRequest*)target);
}
void deleteRequest(ApiRequest* request) {
    if(MessageBoxW(gWindow,(L"确定删除接口“"+toWide(request->name)+L"”吗？").c_str(),L"删除接口",MB_YESNO|MB_ICONWARNING)!=IDYES)return;
    auto folder=findFolderForRequest(request);size_t requestIndex=0;if(folder)for(;requestIndex<folder->requests.size()&&folder->requests[requestIndex].get()!=request;++requestIndex){}
    for(int i=(int)gTabs.size()-1;i>=0;--i)if(gTabs[(size_t)i]->request==request)closeTab(i,false);
    if(folder)folder->requests.erase(std::remove_if(folder->requests.begin(),folder->requests.end(),[&](auto& p){return p.get()==request;}),folder->requests.end());
    rebuildTree();if(gTabs.empty()&&folder&&!folder->requests.empty())openRequest(folder->requests[std::min(requestIndex,folder->requests.size()-1)].get());setSaveStatus(L"未保存");
}
void deleteCase(ApiRequest* request,ApiRequestCase* requestCase) {
    if(!request||!requestCase)return;
    if(MessageBoxW(gWindow,(L"确定删除用例“"+toWide(requestCase->name)+L"”吗？").c_str(),L"删除用例",MB_YESNO|MB_ICONWARNING)!=IDYES)return;
    for(int i=(int)gTabs.size()-1;i>=0;--i)if(gTabs[(size_t)i]->requestCase==requestCase)closeTab(i,false);
    request->cases.erase(std::remove_if(request->cases.begin(),request->cases.end(),[&](auto& item){return item.get()==requestCase;}),request->cases.end());
    rebuildTree();setSaveStatus(L"未保存");
}
void closeFolderTabs(ApiFolder& folder){for(auto& r:folder.requests)for(int i=(int)gTabs.size()-1;i>=0;--i)if(gTabs[(size_t)i]->request==r.get())closeTab(i,false);for(auto& c:folder.children)closeFolderTabs(*c);}
void deleteFolder(ApiFolder* folder) {
    size_t requestCount=0;std::function<void(ApiFolder&)> count=[&](ApiFolder& item){requestCount+=item.requests.size();for(auto& child:item.children)count(*child);};count(*folder);
    wstring message=requestCount?L"目录“"+toWide(folder->name)+L"”及其子目录中包含 "+std::to_wstring(requestCount)+L" 个接口，删除后接口也会一并删除。是否继续？":L"确定删除目录“"+toWide(folder->name)+L"”吗？";
    if(MessageBoxW(gWindow,message.c_str(),L"删除目录",MB_YESNO|MB_ICONWARNING)!=IDYES)return;closeFolderTabs(*folder);
    std::function<bool(std::vector<std::unique_ptr<ApiFolder>>&)> remove=[&](auto& list){auto it=std::find_if(list.begin(),list.end(),[&](auto& p){return p.get()==folder;});if(it!=list.end()){list.erase(it);return true;}for(auto& f:list)if(remove(f->children))return true;return false;};remove(gData.folders);gSelectedFolder=nullptr;
    if(gData.folders.empty()){auto replacement=std::make_unique<ApiFolder>();replacement->id=newId();replacement->name="默认目录";gSelectedFolder=replacement.get();gData.folders.push_back(std::move(replacement));}
    if(!gSelectedFolder)gSelectedFolder=gData.folders.front().get();rebuildTree();selectTreeValue(NodeRef::Kind::Folder,gSelectedFolder);setSaveStatus(L"未保存");
}
void duplicateRequest(ApiRequest* request) {
    auto folder=findFolderForRequest(request);if(!folder)return;auto copy=cloneRequest(*request);ApiRequest* pointer=copy.get();auto position=std::find_if(folder->requests.begin(),folder->requests.end(),[&](const auto& item){return item.get()==request;});
    folder->requests.insert(position==folder->requests.end()?folder->requests.end():position+1,std::move(copy));rebuildTree();openRequest(pointer);setSaveStatus(L"未保存");
}
void duplicateSelectedTab() {
    commitCellEditor();saveEditor();auto tab=selectedTab();if(!tab)return;
    if(!tab->caseSnapshot){duplicateRequest(tab->request);return;}
    auto folder=findFolderForRequest(tab->request);if(!folder)return;auto copy=cloneRequest(*tab->caseSnapshot);ApiRequest* pointer=copy.get();
    folder->requests.insert(folder->requests.begin(),std::move(copy));rebuildTree();openRequest(pointer);setSaveStatus(L"未保存");
}
void moveRequest(ApiRequest* request) {
    ApiFolder* source=findFolderForRequest(request);std::vector<FolderChoice> choices;std::function<void(ApiFolder&)> collect=[&](ApiFolder& f){
        if(&f!=source){FolderChoice choice;choice.folder=&f;choice.label=toWide(f.name);choices.push_back(std::move(choice));}
        for(auto& child:f.children)collect(*child);
    };for(auto& f:gData.folders)collect(*f);
    ApiFolder* target=nullptr;if(!pickFolder(choices,target))return;
    std::unique_ptr<ApiRequest> moving;for(auto it=source->requests.begin();it!=source->requests.end();++it)if(it->get()==request){moving=std::move(*it);source->requests.erase(it);break;}target->requests.push_back(std::move(moving));gSelectedFolder=target;rebuildTree();openRequest(request);setSaveStatus(L"未保存");
}
void moveRequestDirect(ApiRequest* request,ApiFolder* target) {
    ApiFolder* source=findFolderForRequest(request);if(!source||!target||source==target)return;
    std::unique_ptr<ApiRequest> moving;
    for(auto it=source->requests.begin();it!=source->requests.end();++it)if(it->get()==request){moving=std::move(*it);source->requests.erase(it);break;}
    if(!moving)return;target->requests.push_back(std::move(moving));gSelectedFolder=target;rebuildTree();openRequest(request);setSaveStatus(L"未保存");
}

void showTreeMenu(POINT point) {
    if(!gTreeSelection)return;HMENU menu=CreatePopupMenu();
    if(gTreeSelection->kind==NodeRef::Kind::Folder){AppendMenuW(menu,MF_STRING,IDM_FOLDER_ADD_REQUEST,L"＋ 新增接口");AppendMenuW(menu,MF_STRING,IDM_FOLDER_ADD_CHILD,L"＋ 新增子目录");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,gImporting?MF_GRAYED:MF_STRING,IDM_FOLDER_IMPORT,L"导入 OpenAPI/Swagger");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,IDM_FOLDER_RENAME,L"重命名\tF2");AppendMenuW(menu,MF_STRING,IDM_FOLDER_DELETE,L"删除目录\tDelete");}
    else if(gTreeSelection->kind==NodeRef::Kind::Request){AppendMenuW(menu,MF_STRING,IDM_REQUEST_OPEN,L"打开接口");AppendMenuW(menu,MF_STRING,IDM_REQUEST_RENAME,L"重命名\tF2");AppendMenuW(menu,MF_STRING,IDM_REQUEST_DUPLICATE,L"复制接口\tCtrl+D");AppendMenuW(menu,gData.folders.size()>1?MF_STRING:MF_GRAYED,IDM_REQUEST_MOVE,L"移动接口");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,IDM_REQUEST_DELETE,L"删除接口\tDelete");}
    else {AppendMenuW(menu,MF_STRING,IDM_REQUEST_OPEN,L"打开用例");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,IDM_CASE_DELETE,L"删除用例\tDelete");}
    TrackPopupMenu(menu,TPM_RIGHTBUTTON,point.x,point.y,0,gWindow,nullptr);DestroyMenu(menu);
}
void importOpenApi(ApiFolder* folder) {
    if(!folder||gImporting)return;wstring url;
    if(!prompt(L"导入 OpenAPI/Swagger",L"请输入 Swagger/OpenAPI JSON 地址：",L"https://localhost:7297/swagger/v1/swagger.json",url))return;
    gImporting=true;gImportCancel=false;
    RequestSnapshot request;request.method="GET";request.url=toUtf8(url);
    string targetId=folder->id;gImportWorker=std::thread([targetId=std::move(targetId),request=std::move(request)](){
        auto download=new ImportDownload();download->targetId=targetId;download->sourceUrl=request.url;download->result=executeHttp(request,gImportCancel,&gImportRequest);PostMessageW(gWindow,WM_IMPORT_DONE,(WPARAM)download,0);
    });
}

void editListCell(int row,int column,bool keyboard=false);
void toggleEntry(int row);
void ensureTrailingEntryRow();
wstring entryCellText(int row,int column) {
    return listCellText(gKvList,row,column);
}
void selectEntryFile(int row) {
    wchar_t path[32768]{};wstring current=entryCellText(row,2);if(current.size()<std::size(path))wcscpy_s(path,current.c_str());
    OPENFILENAMEW dialog{sizeof(dialog)};dialog.hwndOwner=gWindow;dialog.lpstrFile=path;dialog.nMaxFile=(DWORD)std::size(path);dialog.lpstrFilter=L"All files\0*.*\0\0";dialog.nFilterIndex=1;dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER;
    if(!GetOpenFileNameW(&dialog))return;ListView_SetItemText(gKvList,row,2,path);ensureTrailingEntryRow();saveEditor();scheduleSave();
}
void copySelectedListRow(HWND list,bool enabled) {
    int row=ListView_GetNextItem(list,-1,LVNI_SELECTED);if(row<0)return;wstring text;
    if(enabled)text=ListView_GetCheckState(list,row)?L"True\t":L"False\t";
    int offset=enabled?1:0;text+=listCellText(list,row,offset);text+=L'\t';text+=listCellText(list,row,offset+1);
    if(enabled){for(int column=3;column<=4;++column){text+=L'\t';text+=listCellText(list,row,column);}}
    if(!OpenClipboard(gWindow))return;EmptyClipboard();size_t bytes=(text.size()+1)*sizeof(wchar_t);HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
    if(memory){void* target=GlobalLock(memory);if(target){memcpy(target,text.c_str(),bytes);GlobalUnlock(memory);if(!SetClipboardData(CF_UNICODETEXT,memory))GlobalFree(memory);}else GlobalFree(memory);}CloseClipboard();
}
void addEntryRow() {
    int index=ListView_GetItemCount(gKvList);LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=index;item.pszText=(LPWSTR)L"";bool previousLoading=gLoadingEntryList;gLoadingEntryList=true;ListView_InsertItem(gKvList,&item);ListView_SetCheckState(gKvList,index,TRUE);gLoadingEntryList=previousLoading;for(int column=1;column<=4;++column)ListView_SetItemText(gKvList,index,column,(LPWSTR)L"");ListView_SetItemState(gKvList,index,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
}
bool entryRowHasText(int row) {
    for(int column=1;column<=4;++column){if(!entryCellText(row,column).empty())return true;}return false;
}
bool isEntryPlaceholder(int row) {return row==ListView_GetItemCount(gKvList)-1&&!entryRowHasText(row);}
RECT entryCellBounds(int row,int column) {
    RECT bounds{};ListView_GetSubItemRect(gKvList,row,column,LVIR_BOUNDS,&bounds);
    if(column==0){RECT header{};Header_GetItemRect(ListView_GetHeader(gKvList),0,&header);MapWindowPoints(ListView_GetHeader(gKvList),gKvList,(POINT*)&header,2);bounds.left=header.left;bounds.right=header.right;}
    return bounds;
}
LRESULT drawEntryList(NMLVCUSTOMDRAW* draw) {
    if(draw->nmcd.dwDrawStage==CDDS_PREPAINT)return CDRF_NOTIFYITEMDRAW;
    if(draw->nmcd.dwDrawStage!=CDDS_ITEMPREPAINT)return CDRF_DODEFAULT;
    const int row=(int)draw->nmcd.dwItemSpec;HDC dc=draw->nmcd.hdc;int saved=SaveDC(dc);
    RECT bounds{};ListView_GetItemRect(gKvList,row,&bounds,LVIR_BOUNDS);
    bool placeholder=isEntryPlaceholder(row),checked=ListView_GetCheckState(gKvList,row)!=FALSE;
    bool focused=GetFocus()==gKvList&&row==ListView_GetNextItem(gKvList,-1,LVNI_SELECTED);
    bool editing=gCellEditor&&row==gEditRow,hot=row==gEntryHotRow;
    COLORREF background=focused||editing?RGB(242,247,255):(hot?RGB(248,250,253):RGB(255,255,255));
    HBRUSH brush=CreateSolidBrush(background);FillRect(dc,&bounds,brush);DeleteObject(brush);SetBkMode(dc,TRANSPARENT);
    for(int column=0;column<6;++column){
        RECT cell=entryCellBounds(row,column);if(!RectVisible(dc,&cell))continue;
        int cellState=SaveDC(dc);IntersectClipRect(dc,cell.left,cell.top,cell.right,cell.bottom);
        int cx=(cell.left+cell.right)/2,cy=(cell.top+cell.bottom)/2;
        if(column==0){
            if(placeholder){
                HPEN pen=CreatePen(PS_SOLID,px(1),COLOR_ACCENT);auto old=SelectObject(dc,pen);
                MoveToEx(dc,cx-px(4),cy,nullptr);LineTo(dc,cx+px(4)+1,cy);MoveToEx(dc,cx,cy-px(4),nullptr);LineTo(dc,cx,cy+px(4)+1);SelectObject(dc,old);DeleteObject(pen);
            }else{
                RECT box{cx-px(7),cy-px(7),cx+px(7),cy+px(7)};
                HBRUSH boxBrush=CreateSolidBrush(checked?RGB(235,242,255):RGB(255,255,255));HPEN pen=CreatePen(PS_SOLID,1,checked?RGB(161,189,244):RGB(199,207,218));
                auto oldBrush=SelectObject(dc,boxBrush),oldPen=SelectObject(dc,pen);RoundRect(dc,box.left,box.top,box.right,box.bottom,px(4),px(4));SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(boxBrush);DeleteObject(pen);
                if(checked){pen=CreatePen(PS_SOLID,px(1),COLOR_ACCENT);oldPen=SelectObject(dc,pen);MoveToEx(dc,cx-px(4),cy,nullptr);LineTo(dc,cx-px(1),cy+px(3));LineTo(dc,cx+px(4),cy-px(3));SelectObject(dc,oldPen);DeleteObject(pen);}
            }
        }else if(column==5){
            if(!placeholder&&(hot||focused||editing)){
                bool deleteHot=hot&&gEntryHotColumn==5;RECT button{cx-px(11),cy-px(11),cx+px(11),cy+px(11)};
                if(deleteHot)fillRoundRect(dc,button,RGB(254,237,237),6);
                HPEN pen=CreatePen(PS_SOLID,px(1),deleteHot?RGB(207,65,65):COLOR_SECONDARY);auto old=SelectObject(dc,pen);
                MoveToEx(dc,cx-px(3),cy-px(3),nullptr);LineTo(dc,cx+px(3)+1,cy+px(3)+1);MoveToEx(dc,cx+px(3),cy-px(3),nullptr);LineTo(dc,cx-px(3)-1,cy+px(3)+1);SelectObject(dc,old);DeleteObject(pen);
            }
        }else{
            cell.left+=px(12);cell.right-=px(12);wstring text=entryCellText(row,column);COLORREF color=checked?COLOR_PRIMARY:RGB(139,149,164);
            SelectObject(dc,gUiFont);
            if(placeholder){if(column==1){text=gEditorPage==1?L"添加请求头…":L"添加参数…";color=COLOR_ACCENT;}else text.clear();}
            else if(column==3&&!text.empty()){
                COLORREF tint=RGB(243,245,248);color=RGB(94,111,133);
                if(_wcsicmp(text.c_str(),L"number")==0){tint=RGB(237,243,255);color=RGB(77,112,177);}
                else if(_wcsicmp(text.c_str(),L"file")==0){tint=RGB(255,246,229);color=RGB(157,113,48);}
                if(!checked){tint=RGB(247,248,250);color=RGB(139,149,164);}
                SelectObject(dc,gEntryTypeFont);SIZE size{};GetTextExtentPoint32W(dc,text.c_str(),(int)text.size(),&size);
                RECT badge{cell.left,cy-px(10),std::min(cell.right,cell.left+size.cx+px(16)),cy+px(10)};
                if(badge.right>badge.left){fillRoundRect(dc,badge,tint,6);cell=badge;cell.left+=px(8);cell.right-=px(8);}
            }else if(column==4)color=checked?COLOR_SECONDARY:RGB(139,149,164);
            else if(column==2&&text.empty()){text=L"—";color=RGB(185,194,206);}
            SetTextColor(dc,color);DrawTextW(dc,text.c_str(),(int)text.size(),&cell,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
        }
        RestoreDC(dc,cellState);
    }
    RECT line{bounds.left+px(12),bounds.bottom-1,bounds.right-px(12),bounds.bottom};HBRUSH lineBrush=CreateSolidBrush(RGB(239,242,246));FillRect(dc,&line,lineBrush);DeleteObject(lineBrush);
    if(focused){RECT cell=entryCellBounds(row,gFocusedEntryColumn);InflateRect(&cell,-px(3),-px(3));SetTextColor(dc,COLOR_ACCENT);DrawFocusRect(dc,&cell);}
    RestoreDC(dc,saved);return CDRF_SKIPDEFAULT;
}
void ensureTrailingEntryRow() {
    int count=ListView_GetItemCount(gKvList);while(count>1&&!entryRowHasText(count-1)&&!entryRowHasText(count-2)){ListView_DeleteItem(gKvList,--count);}if(count==0||entryRowHasText(count-1))addEntryRow();
}
void deleteEntryRow(int row=-1){if(row<0)row=ListView_GetNextItem(gKvList,-1,LVNI_SELECTED);if(row>=0){gEntryHotRow=gEntryHotColumn=-1;bool previousLoading=gLoadingEntryList;gLoadingEntryList=true;ListView_DeleteItem(gKvList,row);gLoadingEntryList=previousLoading;ensureTrailingEntryRow();saveEditor();scheduleSave();}}
void commitCellEditor(bool save) {
    if(!gCellEditor)return;HWND editor=gCellEditor;gCellEditor=nullptr;
    if(save&&gEditRow>=0&&gEditColumn>=0){wstring value=textOf(editor);ListView_SetItemText(gKvList,gEditRow,gEditColumn,(LPWSTR)value.c_str());}
    int editedRow=gEditRow;DestroyWindow(editor);gEditRow=gEditColumn=-1;
    if(save&&editedRow>=0){if(entryCellText(editedRow,3).empty()&&(!entryCellText(editedRow,1).empty()||!entryCellText(editedRow,2).empty()))ListView_SetItemText(gKvList,editedRow,3,(LPWSTR)L"string");ensureTrailingEntryRow();}
    saveEditor();if(save)scheduleSave();
}
void navigateEntry(bool backwards) {
    int row=gEditRow,column=gEditColumn;
    if(row<0){row=ListView_GetNextItem(gKvList,-1,LVNI_SELECTED);column=gFocusedEntryColumn;}
    if(row<0){moveFocus(gKvList,backwards);return;}
    commitCellEditor();
    EntryCell next=nextEntryCell(row,column,ListView_GetItemCount(gKvList),backwards);
    if(next.row<0){moveFocus(gKvList,backwards);return;}
    editListCell(next.row,next.column,true);
}
LRESULT CALLBACK cellEditorProc(HWND h,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
    if(message==WM_IME_STARTCOMPOSITION)gImeComposing=true;
    if(message==WM_IME_ENDCOMPOSITION)gImeComposing=false;
    if(message==WM_KEYDOWN&&!gImeComposing){
        if(w==VK_TAB){navigateEntry(GetKeyState(VK_SHIFT)<0);return 0;}
        if(w==VK_RETURN||w==VK_ESCAPE){
            int row=gEditRow;gFocusedEntryColumn=gEditColumn;commitCellEditor(w!=VK_ESCAPE);
            ListView_SetItemState(gKvList,row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);SetFocus(gKvList);return 0;
        }
    }
    if(message==WM_KILLFOCUS&&gCellEditor&&(HWND)w!=gCellEditor&&!IsChild(gCellEditor,(HWND)w)){commitCellEditor();return 0;}
    return DefSubclassProc(h,message,w,l);
}
LRESULT CALLBACK kvListProc(HWND h,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
    if(message==WM_MOUSEMOVE){
        LVHITTESTINFO hit{};hit.pt={(short)LOWORD(l),(short)HIWORD(l)};ListView_SubItemHitTest(h,&hit);
        if(gEntryHotRow!=hit.iItem||gEntryHotColumn!=hit.iSubItem){int previous=gEntryHotRow;gEntryHotRow=hit.iItem;gEntryHotColumn=hit.iSubItem;if(previous>=0)ListView_RedrawItems(h,previous,previous);if(hit.iItem>=0)ListView_RedrawItems(h,hit.iItem,hit.iItem);}
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);
    }
    if(message==WM_MOUSELEAVE||message==WM_MOUSEWHEEL||message==WM_VSCROLL||message==WM_HSCROLL){
        if(message!=WM_MOUSELEAVE)commitCellEditor();int previous=gEntryHotRow;gEntryHotRow=gEntryHotColumn=-1;if(previous>=0)ListView_RedrawItems(h,previous,previous);
    }
    if(message==WM_NOTIFY){auto header=(NMHDR*)l;if(header->hwndFrom==ListView_GetHeader(h)&&(header->code==HDN_BEGINTRACKW||header->code==HDN_BEGINTRACKA))commitCellEditor();}
    if(message==WM_SETFOCUS||message==WM_KILLFOCUS)InvalidateRect(h,nullptr,FALSE);
    if(message==WM_KEYDOWN) {
        int row=ListView_GetNextItem(h,-1,LVNI_SELECTED);
        if(w==VK_TAB){navigateEntry(GetKeyState(VK_SHIFT)<0);return 0;}
        if(w==VK_LEFT||w==VK_RIGHT){gFocusedEntryColumn=std::clamp(gFocusedEntryColumn+(w==VK_LEFT?-1:1),1,4);InvalidateRect(h,nullptr,FALSE);return 0;}
        if(w==VK_INSERT){int count=ListView_GetItemCount(gKvList);if(count>0)editListCell(count-1,1);return 0;}
        if(w==VK_DELETE){deleteEntryRow();return 0;}
        if(GetKeyState(VK_CONTROL)<0&&w=='C'){copySelectedListRow(h,true);return 0;}
        if((w==VK_RETURN||w==VK_F2)&&row>=0){editListCell(row,gFocusedEntryColumn,true);return 0;}
        if(w==VK_SPACE&&row>=0){if(isEntryPlaceholder(row))editListCell(row,1,true);else toggleEntry(row);return 0;}
    }
    if(message==WM_LBUTTONDOWN) {
        LVHITTESTINFO hit{};hit.pt={(short)LOWORD(l),(short)HIWORD(l)};ListView_SubItemHitTest(h,&hit);
        if(hit.iItem>=0&&hit.iSubItem==0){if(!isEntryPlaceholder(hit.iItem))toggleEntry(hit.iItem);return 0;}
    }
    if(message==WM_LBUTTONUP) {
        // Let the list view finish its native click handling first, then open the
        // cell editor.  Creating it from NM_CLICK can make the list reclaim the
        // focus immediately, so the editor disappears before text can be typed.
        LVHITTESTINFO hit{};hit.pt={(short)LOWORD(l),(short)HIWORD(l)};ListView_SubItemHitTest(h,&hit);if(hit.iItem>=0&&hit.iSubItem==0){if(isEntryPlaceholder(hit.iItem))PostMessageW(gWindow,WM_EDIT_ENTRY_CELL,(WPARAM)hit.iItem,1);return 0;}
        if(hit.iItem>=0&&hit.iSubItem==5){if(!isEntryPlaceholder(hit.iItem))deleteEntryRow(hit.iItem);return 0;}
        LRESULT result=DefSubclassProc(h,message,w,l);
        if(hit.iItem>=0&&hit.iSubItem>=1&&hit.iSubItem<=4)PostMessageW(gWindow,WM_EDIT_ENTRY_CELL,(WPARAM)hit.iItem,(LPARAM)hit.iSubItem);
        return result;
    }
    return DefSubclassProc(h,message,w,l);
}
LRESULT CALLBACK responseHeadersProc(HWND h,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
    if(message==WM_KEYDOWN&&GetKeyState(VK_CONTROL)<0&&w=='C'){copySelectedListRow(h,false);return 0;}
    return DefSubclassProc(h,message,w,l);
}
void layoutResponseFindControls() {
    RECT panel{};GetClientRect(gResponseFindPanel,&panel);int width=dip(panel.right);
    HDC dc=GetDC(gFindStatus);auto oldFont=(HFONT)SelectObject(dc,gUiFont);SIZE statusSize{};wstring status=textOf(gFindStatus);
    GetTextExtentPoint32W(dc,status.c_str(),(int)status.size(),&statusSize);SelectObject(dc,oldFont);ReleaseDC(gFindStatus,dc);
    int statusWidth=std::clamp(dip(statusSize.cx)+12,48,154),editWidth=std::max(1,width-28-statusWidth-88);
    MoveWindow(gResponseFindEdit,px(28),px(6),px(editWidth),px(22),TRUE);
    MoveWindow(gFindStatus,px(28+editWidth),px(2),px(statusWidth),px(28),TRUE);
    MoveWindow(gResponseFindPrev,px(width-88),px(2),px(28),px(28),TRUE);
    MoveWindow(gResponseFindNext,px(width-60),px(2),px(28),px(28),TRUE);
    MoveWindow(gResponseFindClose,px(width-32),px(2),px(28),px(28),TRUE);
}
void updateResponseFindStatus() {
    auto current=std::lower_bound(gResponseMatches.begin(),gResponseMatches.end(),gResponseFindPosition);
    bool found=current!=gResponseMatches.end()&&*current==gResponseFindPosition;
    wstring status=gResponseFindQuery.empty()?L"":L"未找到";
    if(found)status=(gResponseFindWrapped?L"↻ ":L"")+std::to_wstring(current-gResponseMatches.begin()+1)+L" / "+std::to_wstring(gResponseMatches.size());
    setText(gFindStatus,status);EnableWindow(gResponseFindPrev,found);EnableWindow(gResponseFindNext,found);
    layoutResponseFindControls();
    InvalidateRect(gFindStatus,nullptr,TRUE);
}
void paintResponseMatches() {
    if(!gRichEditModule||gApplyingResponseHighlights)return;
    std::vector<size_t> visible;
    if(gResponseFindVisible&&gResponsePage==0&&!gResponseMatches.empty()){
        // Only decorate visible portions of lines. Even a multi-megabyte raw
        // line can have millions of matches outside the horizontal viewport.
        RECT viewport{};SendMessageW(gResponseBody,EM_GETRECT,0,(LPARAM)&viewport);
        if(viewport.right>viewport.left&&viewport.bottom>viewport.top){
            POINTL bottom{viewport.right-1,viewport.bottom-1};
            LRESULT lastChar=SendMessageW(gResponseBody,EM_CHARFROMPOS,0,(LPARAM)&bottom);
            int firstLine=(int)SendMessageW(gResponseBody,EM_GETFIRSTVISIBLELINE,0,0);
            int lastLine=(int)SendMessageW(gResponseBody,EM_EXLINEFROMCHAR,0,lastChar);
            for(int line=firstLine;line<=lastLine;++line){
                LRESULT start=SendMessageW(gResponseBody,EM_LINEINDEX,line,0);if(start<0)break;
                POINTL origin{};SendMessageW(gResponseBody,EM_POSFROMCHAR,(WPARAM)&origin,start);
                LONG y=std::clamp(origin.y,viewport.top,viewport.bottom-1);
                POINTL left{viewport.left,y},right{viewport.right-1,y};
                LRESULT begin=SendMessageW(gResponseBody,EM_CHARFROMPOS,0,(LPARAM)&left);
                LRESULT end=SendMessageW(gResponseBody,EM_CHARFROMPOS,0,(LPARAM)&right);
                if(begin<0||end<begin)continue;
                size_t earliest=(size_t)begin>=gResponseFindQuery.size()?(size_t)begin-gResponseFindQuery.size()+1:0;
                for(auto match=std::lower_bound(gResponseMatches.begin(),gResponseMatches.end(),earliest);match!=gResponseMatches.end()&&*match<=(size_t)end;++match)visible.push_back(*match);
            }
        }
        if(gResponseFindPosition!=wstring::npos)visible.push_back(gResponseFindPosition);
        std::sort(visible.begin(),visible.end());visible.erase(std::unique(visible.begin(),visible.end()),visible.end());
    }
    if(visible==gResponseHighlights&&gResponseHighlightLength==gResponseFindQuery.size()&&gResponseHighlightCurrent==gResponseFindPosition)return;
    gApplyingResponseHighlights=true;
    CHARRANGE selection{};POINT scroll{};
    SendMessageW(gResponseBody,EM_EXGETSEL,0,(LPARAM)&selection);SendMessageW(gResponseBody,EM_GETSCROLLPOS,0,(LPARAM)&scroll);
    LRESULT modified=SendMessageW(gResponseBody,EM_GETMODIFY,0,0);
    SendMessageW(gResponseBody,WM_SETREDRAW,FALSE,0);
    CHARFORMAT2W format{};format.cbSize=sizeof(format);format.dwMask=CFM_BACKCOLOR;format.dwEffects=CFE_AUTOBACKCOLOR;
    for(size_t position:gResponseHighlights){
        SendMessageW(gResponseBody,EM_SETSEL,position,position+gResponseHighlightLength);
        SendMessageW(gResponseBody,EM_SETCHARFORMAT,SCF_SELECTION,(LPARAM)&format);
    }
    format.dwEffects=0;
    for(size_t position:visible){
        format.crBackColor=position==gResponseFindPosition?RGB(255,203,87):RGB(255,240,171);
        SendMessageW(gResponseBody,EM_SETSEL,position,position+gResponseFindQuery.size());
        SendMessageW(gResponseBody,EM_SETCHARFORMAT,SCF_SELECTION,(LPARAM)&format);
    }
    SendMessageW(gResponseBody,EM_EXSETSEL,0,(LPARAM)&selection);SendMessageW(gResponseBody,EM_SETSCROLLPOS,0,(LPARAM)&scroll);
    SendMessageW(gResponseBody,EM_SETMODIFY,modified,0);SendMessageW(gResponseBody,WM_SETREDRAW,TRUE,0);
    gResponseHighlights=std::move(visible);gResponseHighlightLength=gResponseFindQuery.size();gResponseHighlightCurrent=gResponseFindPosition;
    gApplyingResponseHighlights=false;
    // Formatting with redraw disabled invalidates Rich Edit's cached pixels.
    // Even inside WM_PAINT, repaint the full client: a scroll's
    // original dirty strip no longer covers everything that needs refreshing.
    InvalidateRect(gResponseBody,nullptr,TRUE);
}
void selectResponseMatch() {
    // Rich Edit ignores EM_SCROLLCARET while its selection is hidden. Reveal it
    // for scrolling, then restore our match backgrounds without moving focus.
    bool hideSelection=gRichEditModule&&gResponseFindVisible&&GetFocus()!=gResponseBody;
    if(hideSelection)SendMessageW(gResponseBody,EM_HIDESELECTION,FALSE,0);
    size_t end=gResponseFindPosition+gResponseFindQuery.size();
    SendMessageW(gResponseBody,EM_SETSEL,end,end);
    SendMessageW(gResponseBody,EM_SCROLLCARET,0,0);
    // Reveal both ends: scrolling only the caret can leave the start clipped
    // when navigating left. For a match wider than the viewport, show its start.
    SendMessageW(gResponseBody,EM_SETSEL,gResponseFindPosition,gResponseFindPosition);
    SendMessageW(gResponseBody,EM_SCROLLCARET,0,0);
    SendMessageW(gResponseBody,EM_SETSEL,gResponseFindPosition,end);
    if(hideSelection)SendMessageW(gResponseBody,EM_HIDESELECTION,TRUE,0);
}
void refreshResponseFind(bool selectFirst) {
    gResponseMatches.clear();wstring query=gResponseFindQuery;
    for(auto& c:query)c=static_cast<wchar_t>(towlower(c));
    if(!query.empty())for(size_t pos=gResponseSearchText.find(query);pos!=wstring::npos;pos=gResponseSearchText.find(query,pos+query.size()))gResponseMatches.push_back(pos);
    bool valid=std::binary_search(gResponseMatches.begin(),gResponseMatches.end(),gResponseFindPosition);
    if(selectFirst||!valid)gResponseFindPosition=gResponseMatches.empty()?wstring::npos:gResponseMatches.front();
    if(gResponseFindVisible&&gResponsePage==0&&gResponseFindPosition!=wstring::npos&&(selectFirst||!valid)){
        selectResponseMatch();
    }
    gResponseFindWrapped=false;KillTimer(gWindow,FIND_WRAP_TIMER);updateResponseFindStatus();paintResponseMatches();
}
void showResponseFind(bool visible) {
    auto tab=selectedTab();if(!tab)return;
    wstring seed;
    if(visible&&gResponsePage==0&&GetFocus()==gResponseBody){
        DWORD start=0,end=0;SendMessageW(gResponseBody,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
        if(end>start){seed=responseText().substr(start,end-start);if(seed.find_first_of(L"\r\n")!=wstring::npos)seed.clear();}
    }
    captureView();gResponseFindVisible=visible;tab->findVisible=visible;
    bool switchedPage=visible&&gResponsePage!=0;
    if(switchedPage){gResponsePage=0;tab->responsePage=0;TabCtrl_SetCurSel(gResponseTabs,0);showResponsePage();}
    if(!seed.empty()){
        bool loading=gLoadingEditor;gLoadingEditor=true;setText(gResponseFindEdit,seed);gLoadingEditor=loading;gResponseFindQuery=seed;
    }
    setVisible(gResponseFind, !visible&&gResponsePage==0);setVisible(gResponseFindPanel,visible);setVisible(gFindStatus,visible);
    RECT client{};GetClientRect(gWindow,&client);layout((int)client.right,(int)client.bottom);
    if(visible){
        if(!switchedPage)refreshResponseFind(!seed.empty());SetFocus(gResponseFindEdit);SendMessageW(gResponseFindEdit,EM_SETSEL,0,-1);
        if(gRichEditModule)SendMessageW(gResponseBody,EM_HIDESELECTION,TRUE,0);
    }else{
        gResponseFindWrapped=false;KillTimer(gWindow,FIND_WRAP_TIMER);updateResponseFindStatus();paintResponseMatches();
        SetFocus(gResponseBody);if(gRichEditModule)SendMessageW(gResponseBody,EM_HIDESELECTION,FALSE,0);
    }
    captureView();
}
void findInResponseBody(bool forward) {
    if(!selectedTab()||gResponseMatches.empty())return;
    auto current=std::lower_bound(gResponseMatches.begin(),gResponseMatches.end(),gResponseFindPosition);
    size_t index=current==gResponseMatches.end()?0:(size_t)(current-gResponseMatches.begin());
    gResponseFindWrapped=forward?index+1==gResponseMatches.size():index==0;
    index=forward?(index+1)%gResponseMatches.size():(index+gResponseMatches.size()-1)%gResponseMatches.size();
    gResponseFindPosition=gResponseMatches[index];updateResponseFindStatus();
    KillTimer(gWindow,FIND_WRAP_TIMER);if(gResponseFindWrapped)SetTimer(gWindow,FIND_WRAP_TIMER,1500,nullptr);
    selectResponseMatch();paintResponseMatches();captureView();
}
LRESULT CALLBACK responseFindEditProc(HWND h,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
    if(message==WM_IME_STARTCOMPOSITION)gImeComposing=true;
    if(message==WM_IME_ENDCOMPOSITION){LRESULT result=DefSubclassProc(h,message,w,l);gImeComposing=false;gResponseFindQuery=textOf(h);refreshResponseFind(true);captureView();return result;}
    if(message==WM_SETFOCUS||message==WM_KILLFOCUS)InvalidateRect(gResponseFindPanel,nullptr,FALSE);
    if(message==WM_KEYDOWN&&!gImeComposing&&w==VK_RETURN){findInResponseBody(GetKeyState(VK_SHIFT)>=0);return 0;}
    if(message==WM_KEYDOWN&&!gImeComposing&&w==VK_ESCAPE){showResponseFind(false);return 0;}
    if(message==WM_CHAR&&!gImeComposing&&(w==VK_RETURN||w==VK_ESCAPE))return 0;
    if(message==WM_KEYDOWN&&GetKeyState(VK_CONTROL)<0&&w=='F'){SendMessageW(h,EM_SETSEL,0,-1);return 0;}
    return DefSubclassProc(h,message,w,l);
}
LRESULT CALLBACK responseFindPanelProc(HWND h,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_COMMAND||message==WM_DRAWITEM||message==WM_CTLCOLORSTATIC||message==WM_CTLCOLOREDIT)return SendMessageW(gWindow,message,w,l);
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT){
        PAINTSTRUCT paint{};HDC dc=BeginPaint(h,&paint);RECT rect{};GetClientRect(h,&rect);FillRect(dc,&rect,gWhiteBrush);
        HPEN border=CreatePen(PS_SOLID,px(1),GetFocus()==gResponseFindEdit?COLOR_ACCENT:COLOR_BORDER);auto oldPen=(HPEN)SelectObject(dc,border);auto oldBrush=(HBRUSH)SelectObject(dc,GetStockObject(NULL_BRUSH));
        RoundRect(dc,rect.left,rect.top,rect.right,rect.bottom,px(6),px(6));SelectObject(dc,oldPen);DeleteObject(border);
        HPEN icon=CreatePen(PS_SOLID,px(1),COLOR_SECONDARY);oldPen=(HPEN)SelectObject(dc,icon);
        int x=px(8),y=(rect.bottom-px(10))/2;Ellipse(dc,x,y,x+px(9),y+px(9));MoveToEx(dc,x+px(8),y+px(8),nullptr);LineTo(dc,x+px(13),y+px(13));
        SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(icon);EndPaint(h,&paint);return 0;
    }
    return DefWindowProcW(h,message,w,l);
}
LRESULT CALLBACK responseBodyProc(HWND h,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
    if(message==WM_PAINT)paintResponseMatches();
    if(gRichEditModule&&(message==WM_SETFOCUS||message==WM_KILLFOCUS))SendMessageW(h,EM_HIDESELECTION,message==WM_KILLFOCUS&&gResponseFindVisible,0);
    if(message==WM_LBUTTONDBLCLK){
        POINTL point{(short)LOWORD(l),(short)HIWORD(l)};
        LRESULT hit=gRichEditModule?SendMessageW(h,EM_CHARFROMPOS,0,(LPARAM)&point):LOWORD(SendMessageW(h,EM_CHARFROMPOS,0,l));
        wstring value=responseText();
        auto word=[](wchar_t c){return (c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'_';};
        if(hit>=0&&(size_t)hit<value.size()&&word(value[(size_t)hit])){
            size_t begin=(size_t)hit,end=begin+1;while(begin>0&&word(value[begin-1]))--begin;while(end<value.size()&&word(value[end]))++end;
            SetFocus(h);SendMessageW(h,EM_SETSEL,(WPARAM)begin,(LPARAM)end);captureView();return 0;
        }
    }
    if(message==WM_KEYDOWN&&GetFocus()==h&&GetKeyState(VK_CONTROL)<0&&w=='F'){showResponseFind(true);return 0;}
    if(message==WM_KEYDOWN&&GetKeyState(VK_CONTROL)<0&&w=='A'){
        SendMessageW(h,EM_SETSEL,0,-1);
        return 0;
    }
    return DefSubclassProc(h,message,w,l);
}
void editListCell(int row,int column,bool keyboard) {
    if(column<=0||column>=5)return;commitCellEditor();
    if(row<0||row>=ListView_GetItemCount(gKvList))return;
    if(!keyboard&&column==2&&_wcsicmp(entryCellText(row,3).c_str(),L"file")==0){selectEntryFile(row);SetFocus(gKvList);return;}
    ListView_EnsureVisible(gKvList,row,FALSE);
    ListView_SetItemState(gKvList,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
    ListView_SetItemState(gKvList,row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    RECT bounds{};if(!ListView_GetSubItemRect(gKvList,row,column,LVIR_BOUNDS,&bounds))return;MapWindowPoints(gKvList,gWindow,(POINT*)&bounds,2);
    InflateRect(&bounds,-px(8),-px(4));
    wstring current=entryCellText(row,column);bool combo=column==3;
    gCellEditor=CreateWindowExW(WS_EX_CLIENTEDGE,combo?L"COMBOBOX":L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|(combo?(CBS_DROPDOWN|WS_VSCROLL):ES_AUTOHSCROLL),
        bounds.left,bounds.top,bounds.right-bounds.left,combo?px(180):bounds.bottom-bounds.top,gWindow,nullptr,gInstance,nullptr);
    if(!gCellEditor)return;applyFont(gCellEditor);
    if(combo){RECT editor{};GetWindowRect(gCellEditor,&editor);SetWindowPos(gCellEditor,nullptr,bounds.left,(bounds.top+bounds.bottom-(editor.bottom-editor.top))/2,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);}
    else SendMessageW(gCellEditor,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(px(3),px(3)));
    if(combo)for(auto type:{L"string",L"number",L"file"})SendMessageW(gCellEditor,CB_ADDSTRING,0,(LPARAM)type);
    setText(gCellEditor,current);gEditRow=row;gEditColumn=column;gFocusedEntryColumn=column;
    SetWindowSubclass(gCellEditor,cellEditorProc,1,0);
    HWND input=gCellEditor;if(combo){COMBOBOXINFO info{sizeof(info)};if(GetComboBoxInfo(gCellEditor,&info)){input=info.hwndItem;SetWindowSubclass(input,cellEditorProc,1,0);}}
    SendMessageW(input,EM_SETLIMITTEXT,0,0);SetWindowPos(gCellEditor,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
    SetFocus(input);SendMessageW(input,EM_SETSEL,0,-1);
}
void toggleEntry(int row){ListView_SetCheckState(gKvList,row,ListView_GetCheckState(gKvList,row)==FALSE);}

RequestSnapshot snapshotCurrent() {
    commitCellEditor();saveEditor();auto tab=selectedTab();auto& r=tab->caseSnapshot?*tab->caseSnapshot:*tab->request;return {r.method,r.url,r.query,r.headers,r.bodyType,r.body,r.formFields};
}
void formatCurrentJson(bool compact) {
    auto tab=selectedTab();if(!tab)return;string source=toUtf8(textOf(gBody));
    if(trimWide(toWide(source)).empty()){tab->validation=L"JSON 内容不能为空。";setValidationText(tab->validation);return;}
    if(source.size()>2*1024*1024){tab->validation=L"JSON 内容超过 2 MB，无法格式化或压缩。";setValidationText(tab->validation);return;}
    if(auto error=jsonValidationMessage(source,!compact);!error.empty()){tab->validation=std::move(error);setValidationText(tab->validation);return;}
    wstring formatted=toWide(prettyJson(source,compact));
    if(!compact){
        wstring windowsText;windowsText.reserve(formatted.size()+formatted.size()/8);
        for(size_t i=0;i<formatted.size();++i){if(formatted[i]==L'\n'&&(i==0||formatted[i-1]!=L'\r'))windowsText+=L'\r';windowsText+=formatted[i];}
        formatted=std::move(windowsText);
    }
    tab->validation.clear();setValidationText(L"");setText(gBody,formatted);SendMessageW(gBody,EM_SETSEL,0,0);SetFocus(gBody);saveEditor();scheduleSave();
}
void sendCurrent() {
    auto tab=selectedTab();if(!tab||tab->sending||gClosingPrompt)return;
    RequestSnapshot request=snapshotCurrent();captureView();
    wstring url=trimWide(toWide(buildRequestUrl(request)));
    URL_COMPONENTSW parts{};parts.dwStructSize=sizeof(parts);parts.dwSchemeLength=(DWORD)-1;parts.dwHostNameLength=(DWORD)-1;
    wstring error;HWND invalidControl=gUrl;
    if(url.empty()||!WinHttpCrackUrl(url.c_str(),0,0,&parts)||parts.dwHostNameLength==0||
        (parts.nScheme!=INTERNET_SCHEME_HTTP&&parts.nScheme!=INTERNET_SCHEME_HTTPS))error=L"请输入完整的 HTTP 或 HTTPS 请求地址。";
    if(error.empty()&&request.bodyType=="JSON"&&!trimWide(toWide(request.body)).empty()){
        invalidControl=gBody;
        error=request.body.size()>2*1024*1024?L"JSON 内容超过 2 MB，无法发送。":jsonValidationMessage(request.body);
    }
    if(!error.empty()){
        tab->validation=error;
        if(invalidControl==gBody){gEditorPage=tab->editorPage=2;TabCtrl_SetCurSel(gEditorTabs,2);showEditorPage();}
        setValidationText(error);SetFocus(invalidControl);return;
    }
    if(tab->worker.joinable())tab->worker.join();
    tab->cancel=false;tab->sending=true;tab->started=std::chrono::steady_clock::now();tab->summary=L"请求中 · 0 ms";
    tab->validation.clear();tab->oldResponse=tab->responseSummary!=L"暂无响应";
    setValidationText(L"");setVisible(gSend,false);setVisible(gCancel,true);EnableWindow(gCancel,TRUE);setText(gCancel,L"取消");showResponsePage();
    SetTimer(gWindow,REQUEST_TIMER,100,nullptr);
    auto keep=tab;tab->worker=std::thread([keep,request=std::move(request)](){
        auto completion=new HttpCompletion();completion->tab=keep;completion->result=executeHttp(request,keep->cancel,&keep->activeRequest);PostMessageW(gWindow,WM_HTTP_DONE,(WPARAM)completion,0);
    });
}
void cancelCurrent() {
    auto tab=selectedTab();if(!tab||!tab->sending||tab->cancel)return;
    tab->cancelNow();tab->summary=L"正在取消";setText(gCancel,L"正在取消");EnableWindow(gCancel,FALSE);
    setText(gSummary,tab->summary+(tab->oldResponse?L" · 正在显示上次响应":L""));
}
void moveFocus(HWND from,bool backwards) {
    std::vector<HWND> order={gSearch,gAddFolder,gTree,gRequestTabs,gSave,gSaveMore,gMethod,gUrl,gSend,gCancel,
        gEditorTabs,gBodyType,gFormat,gCompress,gKvList,gBody,gResponseTabs,gResponseMode,gResponseFind,
        gResponseFindEdit,gResponseFindPrev,gResponseFindNext,gResponseFindClose,gResponseBody,gResponseHeaders};
    order.erase(std::remove_if(order.begin(),order.end(),[](HWND control){return !control||!IsWindowVisible(control)||!IsWindowEnabled(control);}),order.end());
    if(order.empty())return;auto current=std::find(order.begin(),order.end(),from);
    int index=current==order.end()?(backwards?0:-1):(int)std::distance(order.begin(),current);
    index=(index+(backwards?-1:1)+(int)order.size())%(int)order.size();SetFocus(order[(size_t)index]);
}
void switchRequestTab(bool backwards) {
    if(gTabs.empty())return;commitCellEditor();saveEditor();captureView();
    gSelectedTab=(gSelectedTab+(backwards?-1:1)+(int)gTabs.size())%(int)gTabs.size();
    refreshRequestTabs();loadEditor();selectTreeTab(selectedTab());
}

HWND child(const wchar_t* cls,const wchar_t* label,DWORD style,int id,DWORD ex=0) {
    bool focusable=wcscmp(cls,L"STATIC")!=0&&wcscmp(cls,L"SCROLLBAR")!=0;
    HWND h=CreateWindowExW(ex,cls,label,WS_CHILD|WS_VISIBLE|(focusable?WS_TABSTOP:0)|style,0,0,10,10,gWindow,(HMENU)(INT_PTR)id,gInstance,nullptr);applyFont(h);return h;
}
LRESULT CALLBACK searchProc(HWND h,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
    if(message==WM_CHAR&&w==VK_RETURN)return 0;
    LRESULT result=DefSubclassProc(h,message,w,l);
    if(message==WM_PAINT){
        HDC dc=GetDC(h);HPEN pen=CreatePen(PS_SOLID,px(1),COLOR_SECONDARY);auto oldPen=(HPEN)SelectObject(dc,pen);auto oldBrush=(HBRUSH)SelectObject(dc,GetStockObject(NULL_BRUSH));
        RECT client{};GetClientRect(h,&client);int centerY=client.bottom/2;Ellipse(dc,px(9),centerY-px(5),px(19),centerY+px(5));MoveToEx(dc,px(17),centerY+px(3),nullptr);LineTo(dc,px(22),centerY+px(8));
        SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(pen);ReleaseDC(h,dc);
    }
    return result;
}
LRESULT CALLBACK urlProc(HWND h,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
    if(message==WM_CHAR&&w==VK_RETURN)return 0;
    if(message==WM_SETFOCUS||message==WM_KILLFOCUS){InvalidateRect(gMethod,nullptr,FALSE);InvalidateRect(gUrlFrame,nullptr,FALSE);InvalidateRect(h,nullptr,FALSE);}
    LRESULT result=DefSubclassProc(h,message,w,l);
    if(message==WM_PAINT&&GetFocus()!=h&&GetWindowTextLengthW(h)==0){
        HDC dc=GetDC(h);RECT textRect{};SendMessageW(h,EM_GETRECT,0,(LPARAM)&textRect);HGDIOBJ oldFont=SelectObject(dc,gCodeFont);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(156,163,175));
        DrawTextW(dc,L"https://api.example.com",-1,&textRect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);SelectObject(dc,oldFont);ReleaseDC(h,dc);
    }
    return result;
}
void showMethodMenu() {
    static const wchar_t* methods[]={L"GET",L"POST",L"PUT",L"PATCH",L"DELETE",L"HEAD",L"OPTIONS"};
    HMENU menu=CreatePopupMenu();wstring current=textOf(gMethod);
    for(UINT i=0;i<7;++i)AppendMenuW(menu,MF_STRING|(current==methods[i]?MF_CHECKED:0),i+1,methods[i]);
    RECT bounds{};GetWindowRect(gMethod,&bounds);UINT command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_LEFTALIGN|TPM_TOPALIGN,bounds.left,bounds.bottom,0,gWindow,nullptr);DestroyMenu(menu);
    if(command>=1&&command<=7){setText(gMethod,methods[command-1]);saveEditor();scheduleSave();InvalidateRect(gMethod,nullptr,TRUE);}
}
LRESULT CALLBACK requestToolbarButtonProc(HWND h,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR) {
    if(message==WM_MOUSEMOVE&&IsWindowEnabled(h)&&gRequestToolbarHot!=h){
        HWND previous=gRequestToolbarHot;gRequestToolbarHot=h;
        if(previous)InvalidateRect(previous,nullptr,FALSE);InvalidateRect(h,nullptr,FALSE);
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);
    }
    if(message==WM_MOUSELEAVE||((message==WM_ENABLE||message==WM_SHOWWINDOW)&&!w)||message==WM_NCDESTROY){
        if(gRequestToolbarHot==h){gRequestToolbarHot=nullptr;InvalidateRect(h,nullptr,FALSE);}
    }
    LRESULT result=DefSubclassProc(h,message,w,l);
    if(message==WM_SETFOCUS||message==WM_KILLFOCUS||message==WM_UPDATEUISTATE){
        InvalidateRect(h,nullptr,FALSE);if(h==gMethod)InvalidateRect(gUrlFrame,nullptr,FALSE);
    }
    if(message==WM_NCDESTROY)RemoveWindowSubclass(h,requestToolbarButtonProc,id);
    return result;
}
LRESULT CALLBACK responseToolbarButtonProc(HWND h,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR) {
    if(message==WM_MOUSEMOVE&&IsWindowEnabled(h)&&gResponseToolbarHot!=h){
        HWND previous=gResponseToolbarHot;gResponseToolbarHot=h;
        if(previous)InvalidateRect(previous,nullptr,FALSE);InvalidateRect(h,nullptr,FALSE);
        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);
    }
    if(message==WM_MOUSELEAVE||((message==WM_ENABLE||message==WM_SHOWWINDOW)&&!w)||message==WM_NCDESTROY){
        if(gResponseToolbarHot==h){gResponseToolbarHot=nullptr;InvalidateRect(h,nullptr,FALSE);}
    }
    if(message==WM_NCDESTROY)RemoveWindowSubclass(h,responseToolbarButtonProc,id);
    return DefSubclassProc(h,message,w,l);
}
void createControls() {
    gSearch=child(L"EDIT",L"",ES_MULTILINE|ES_AUTOHSCROLL,IDC_SEARCH,WS_EX_CLIENTEDGE);SetWindowSubclass(gSearch,searchProc,5,0);
    gAddFolder=child(L"BUTTON",L"+",BS_OWNERDRAW,IDC_ADD_FOLDER);
    gSidebarDivider=child(L"STATIC",L"",SS_LEFT,IDC_SIDEBAR_DIVIDER);
    gTree=child(WC_TREEVIEWW,L"",TVS_SHOWSELALWAYS|TVS_FULLROWSELECT|TVS_TRACKSELECT|TVS_NOHSCROLL,IDC_TREE,0);TreeView_SetExtendedStyle(gTree,TVS_EX_DOUBLEBUFFER,TVS_EX_DOUBLEBUFFER);
    // Keep the tab control and its custom scrollbar in adjacent rectangles.
    // Overlapping sibling windows can briefly expose one another while the tab
    // window moves on every drag frame, which makes the thumb appear to flash.
    gRequestTabs=child(WC_TABCONTROLW,L"",TCS_TABS|TCS_SINGLELINE,IDC_REQUEST_TABS);SendMessageW(gRequestTabs,TCM_SETITEMSIZE,0,MAKELPARAM(0,px(REQUEST_TAB_HEIGHT-4)));SendMessageW(gRequestTabs,TCM_SETPADDING,0,MAKELPARAM(px(22),px(4)));SetWindowSubclass(gRequestTabs,requestTabsProc,3,0);
    gRequestTabScroll=child(L"SCROLLBAR",L"",SBS_HORZ,IDC_REQUEST_TAB_SCROLL);SetWindowSubclass(gRequestTabScroll,requestTabScrollProc,8,0);ShowWindow(gRequestTabScroll,SW_HIDE);
    gMethod=child(L"BUTTON",L"GET",BS_OWNERDRAW,IDC_METHOD);
    gUrlFrame=child(L"STATIC",L"",SS_OWNERDRAW,IDC_URL_FRAME);
    const wchar_t* urlClass=gRichEditModule?MSFTEDIT_CLASS:L"EDIT";
    gUrl=child(urlClass,L"",ES_MULTILINE|ES_AUTOHSCROLL,IDC_URL);
    if(gRichEditModule){SendMessageW(gUrl,EM_SETTEXTMODE,TM_PLAINTEXT|TM_MULTILEVELUNDO,0);SendMessageW(gUrl,EM_SETEVENTMASK,0,ENM_CHANGE);}
    applyFont(gUrl,gCodeFont);SendMessageW(gUrl,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(0,0));SetWindowSubclass(gUrl,urlProc,6,0);
    gSave=child(L"BUTTON",L"保存",BS_OWNERDRAW,IDC_SAVE);gSaveMore=child(L"BUTTON",L"更多保存选项",BS_OWNERDRAW,IDC_SAVE_MORE);
    gSaveTooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,gWindow,nullptr,gInstance,nullptr);
    TOOLINFOW saveTool{sizeof(saveTool)};saveTool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;saveTool.hwnd=gWindow;saveTool.uId=(UINT_PTR)gSave;saveTool.lpszText=(LPWSTR)L"保存全部接口、用例及目录修改 (Ctrl+S)";SendMessageW(gSaveTooltip,TTM_ADDTOOLW,0,(LPARAM)&saveTool);
    gSend=child(L"BUTTON",L"发送",BS_OWNERDRAW,IDC_SEND);gCancel=child(L"BUTTON",L"取消",BS_OWNERDRAW,IDC_CANCEL);
    for(HWND button:{gMethod,gSave,gSaveMore,gSend,gCancel})SetWindowSubclass(button,requestToolbarButtonProc,13,0);
    gEditorTabs=child(WC_TABCONTROLW,L"",TCS_TABS|TCS_SINGLELINE|TCS_FIXEDWIDTH,IDC_EDITOR_TABS);SendMessageW(gEditorTabs,TCM_SETITEMSIZE,0,MAKELPARAM(px(SECTION_TAB_WIDTH),px(30)));for(auto label:{L"Params",L"Headers",L"Body"}){TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=(LPWSTR)label;TabCtrl_InsertItem(gEditorTabs,TabCtrl_GetItemCount(gEditorTabs),&item);}
    SetWindowSubclass(gEditorTabs,sectionTabsProc,11,0);
    gKvList=child(WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL,IDC_KV_LIST);ListView_SetExtendedListViewStyle(gKvList,LVS_EX_DOUBLEBUFFER|LVS_EX_CHECKBOXES|LVS_EX_LABELTIP);addListColumns(gKvList,true);
    SetWindowSubclass(gKvList,kvListProc,2,0);
    SetWindowSubclass(ListView_GetHeader(gKvList),entryHeaderProc,12,0);updateEntryMetrics();
    gBodyType=child(L"COMBOBOX",L"",CBS_DROPDOWNLIST,IDC_BODY_TYPE);for(auto type:{L"None",L"JSON",L"Form URL Encoded",L"Multipart Form Data",L"Raw"})SendMessageW(gBodyType,CB_ADDSTRING,0,(LPARAM)type);
    gFormat=child(L"BUTTON",L"格式化",BS_OWNERDRAW,IDC_FORMAT);gCompress=child(L"BUTTON",L"压缩",BS_OWNERDRAW,IDC_COMPRESS);
    gBody=child(L"EDIT",L"",ES_MULTILINE|ES_AUTOVSCROLL|ES_AUTOHSCROLL|WS_VSCROLL|WS_HSCROLL,IDC_BODY,WS_EX_CLIENTEDGE);applyFont(gBody,gCodeFont);SendMessageW(gBody,EM_SETLIMITTEXT,2*1024*1024,0);
    gBodyNone=child(L"STATIC",L"当前请求不发送请求体。",SS_CENTER,IDC_BODY_NONE);
    gValidation=child(L"STATIC",L"",SS_LEFT,IDC_VALIDATION);gSummary=child(L"STATIC",L"暂无响应",SS_LEFT,IDC_SUMMARY);
    gResponseTabs=child(WC_TABCONTROLW,L"",TCS_TABS|TCS_SINGLELINE|TCS_FIXEDWIDTH,IDC_RESPONSE_TABS);SendMessageW(gResponseTabs,TCM_SETITEMSIZE,0,MAKELPARAM(px(SECTION_TAB_WIDTH),px(30)));for(auto label:{L"Body",L"Headers"}){TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=(LPWSTR)label;TabCtrl_InsertItem(gResponseTabs,TabCtrl_GetItemCount(gResponseTabs),&item);}
    SetWindowSubclass(gResponseTabs,sectionTabsProc,11,0);
    gResponseBody=child(gRichEditModule?MSFTEDIT_CLASS:L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_NOHIDESEL|ES_AUTOHSCROLL|WS_VSCROLL|WS_HSCROLL,IDC_RESPONSE_BODY,WS_EX_CLIENTEDGE);applyFont(gResponseBody,gCodeFont);SendMessageW(gResponseBody,EM_SETLIMITTEXT,5*1024*1024,0);SetWindowSubclass(gResponseBody,responseBodyProc,7,0);
    if(gRichEditModule){SendMessageW(gResponseBody,EM_SETBKGNDCOLOR,0,RGB(248,250,252));SendMessageW(gResponseBody,EM_SETTARGETDEVICE,0,1);SendMessageW(gResponseBody,EM_SETUNDOLIMIT,0,0);}
    gResponseModeDivider=child(L"STATIC",L"",SS_OWNERDRAW,IDC_RESPONSE_MODE_DIVIDER);
    gResponseMode=child(L"BUTTON",L"格式化",BS_OWNERDRAW,IDC_RESPONSE_MODE);
    gResponseFind=child(L"BUTTON",L"查找",BS_OWNERDRAW,IDC_RESPONSE_FIND);
    for(HWND button:{gResponseMode,gResponseFind})SetWindowSubclass(button,responseToolbarButtonProc,10,0);
    gResponseHeaders=child(WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL,IDC_RESPONSE_HEADERS,WS_EX_CLIENTEDGE);ListView_SetExtendedListViewStyle(gResponseHeaders,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);addListColumns(gResponseHeaders,false);
    SetWindowSubclass(gResponseHeaders,responseHeadersProc,4,0);
    gResponseFindPanel=CreateWindowExW(WS_EX_CONTROLPARENT,L"FeatherApiResponseFind",L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,10,10,gWindow,(HMENU)(INT_PTR)IDC_RESPONSE_FIND_PANEL,gInstance,nullptr);
    gResponseFindEdit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,0,0,10,10,gResponseFindPanel,(HMENU)(INT_PTR)IDC_RESPONSE_FIND_EDIT,gInstance,nullptr);applyFont(gResponseFindEdit);SetWindowSubclass(gResponseFindEdit,responseFindEditProc,9,0);
    SendMessageW(gResponseFindEdit,EM_SETCUEBANNER,TRUE,(LPARAM)L"在响应中查找");
    SendMessageW(gResponseFindEdit,EM_SETLIMITTEXT,5*1024*1024,0);
    gFindStatus=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_CENTER|SS_CENTERIMAGE,0,0,10,10,gResponseFindPanel,(HMENU)(INT_PTR)IDC_FIND_STATUS,gInstance,nullptr);applyFont(gFindStatus);
    auto findButton=[&](const wchar_t* label,int id){HWND button=CreateWindowW(L"BUTTON",label,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,10,10,gResponseFindPanel,(HMENU)(INT_PTR)id,gInstance,nullptr);applyFont(button);return button;};
    gResponseFindPrev=findButton(L"上一个",IDC_RESPONSE_FIND_PREV);gResponseFindNext=findButton(L"下一个",IDC_RESPONSE_FIND_NEXT);gResponseFindClose=findButton(L"关闭",IDC_RESPONSE_FIND_CLOSE);
    for(HWND control:{gResponseFindPanel,gFindStatus})setVisible(control,false);
    auto tooltip=[&](HWND control,const wchar_t* label){TOOLINFOW tool{sizeof(tool)};tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tool.hwnd=gWindow;tool.uId=(UINT_PTR)control;tool.lpszText=(LPWSTR)label;SendMessageW(gSaveTooltip,TTM_ADDTOOLW,0,(LPARAM)&tool);};
    tooltip(gSend,L"发送请求 (Ctrl+Enter)");tooltip(gUrl,L"请求地址 (Ctrl+L)");tooltip(gResponseFind,L"查找响应 (Ctrl+F)，Enter 下一个，Shift+Enter 上一个，Esc 关闭");
    tooltip(gMethod,L"选择 HTTP 请求方法");tooltip(gSaveMore,L"更多保存选项");tooltip(gCancel,L"取消当前请求");
    tooltip(gResponseMode,L"响应显示方式：格式化 / 原始");
    tooltip(gResponseFindPrev,L"上一个 (Shift+Enter)");tooltip(gResponseFindNext,L"下一个 (Enter)");tooltip(gResponseFindClose,L"关闭查找 (Esc)");tooltip(gFindStatus,L"当前匹配 / 全部匹配；↻ 表示已循环查找");
    tooltip(gKvList,L"Tab / Shift+Tab 连续编辑；Enter 提交；Esc 撤销单元格；F2 编辑；Delete 删除行");tooltip(gAddFolder,L"新增目录");
    gEmptyTitle=child(L"STATIC",L"还没有打开接口",SS_CENTER,IDC_EMPTY_TITLE);applyFont(gEmptyTitle,gTitleFont);
    gEmptyHelp=child(L"STATIC",L"请在左侧选择接口，或右键点击目录，选择“新增接口”。",SS_CENTER,IDC_EMPTY_HELP);
}

void layout(int width,int height) {
    width=dip(width);height=dip(height);
    RECT oldTreeBounds{};
    if(gResizeSidebar&&gTree){GetWindowRect(gTree,&oldTreeBounds);MapWindowPoints(nullptr,gWindow,(POINT*)&oldTreeBounds,2);}
    bool freezeLists=gResizeSidebar&&gKvList;
    if(freezeLists){
        SendMessageW(gKvList,WM_SETREDRAW,FALSE,0);
        SendMessageW(gResponseHeaders,WM_SETREDRAW,FALSE,0);
    }
    // Moving each child with MoveWindow(..., TRUE) makes every control erase and
    // repaint independently.  While a splitter is being dragged that exposes
    // intermediate child surfaces (most noticeably the tree view) as trails.
    // Commit the whole layout as one window-position batch instead.
    HDWP deferred=BeginDeferWindowPos(32);
    auto move=[&](HWND control,int x,int y,int w,int h){
        UINT flags=SWP_NOZORDER|SWP_NOACTIVATE;
        // Only the custom-drawn tree needs copy-bits disabled. Applying this to
        // every child makes owner-drawn buttons erase and repaint on each drag
        // frame, which is visible as flashing.
        if(control==gTree)flags|=SWP_NOCOPYBITS;
        if(deferred)deferred=DeferWindowPos(deferred,control,nullptr,px(x),px(y),px(w),px(h),flags);
    };
    int sidebar=std::clamp(gData.sidebarWidth,180,std::min(420,width-660));int workspaceX=sidebar;int workspaceW=width-workspaceX;
    constexpr int contentInset=10;int contentX=workspaceX+contentInset;int contentW=workspaceW-contentInset*2;
    move(gSearch,12,12,sidebar-64,32);if(!gResizeSidebar)updateSearchFormatting();move(gAddFolder,sidebar-44,12,32,32);move(gSidebarDivider,0,55,sidebar,1);move(gTree,8,64,sidebar-16,height-72);
    constexpr int requestRowTop=REQUEST_BAR_TOP;constexpr int rowHeight=REQUEST_BAR_HEIGHT;constexpr int commandGap=10;constexpr int saveWidth=88;constexpr int saveMoreWidth=24;constexpr int sendWidth=96;
    // Workspace actions stay fixed while request tabs scroll inside their own viewport.
    int tabViewportWidth=contentW-saveWidth-saveMoreWidth-12;
    gRequestTabViewportX=px(contentX);gRequestTabViewportWidth=px(tabViewportWidth);
    gRequestTabContentWidth=std::max(gRequestTabContentWidth,gRequestTabViewportWidth);
    move(gRequestTabs,contentX-dip(gRequestTabScrollOffset),0,dip(gRequestTabContentWidth),REQUEST_TAB_HEIGHT);move(gRequestTabScroll,contentX,REQUEST_TAB_HEIGHT,tabViewportWidth,8);
    int sendX=contentX+contentW-sendWidth;int saveMoreX=contentX+contentW-saveMoreWidth;int saveX=saveMoreX-saveWidth;int urlX=contentX+106;int urlWidth=sendX-commandGap-urlX;
    move(gMethod,contentX,requestRowTop,106,rowHeight);move(gUrlFrame,urlX,requestRowTop,urlWidth,rowHeight);move(gUrl,urlX+12,requestRowTop+6,urlWidth-24,rowHeight-12);
    move(gSend,sendX,requestRowTop,sendWidth,rowHeight);move(gCancel,sendX,requestRowTop,sendWidth,rowHeight);
    int editorTop=REQUEST_EDITOR_TOP;int editorHeight=std::clamp(gData.requestPanelHeight,200,std::max(200,height-350));move(gEditorTabs,contentX,editorTop,TabCtrl_GetItemCount(gEditorTabs)*SECTION_TAB_WIDTH+8,36);
    move(gSave,saveX,(REQUEST_TAB_HEIGHT-28)/2,saveWidth,28);move(gSaveMore,saveMoreX,(REQUEST_TAB_HEIGHT-28)/2,saveMoreWidth,28);
    const int bodyToolbarTop=editorTop+39;
    // A native drop-down list uses its font-defined closed height and ignores
    // the requested 30-DIP height. Center that shorter control in the same
    // toolbar row as the two 30-DIP buttons instead of top-aligning it.
    move(gBodyType,contentX,bodyToolbarTop+3,180,30);move(gFormat,contentX+190,bodyToolbarTop,74,30);move(gCompress,contentX+270,bodyToolbarTop,74,30);
    bool hasValidation=GetWindowTextLengthW(gValidation)>0;int validationReserve=hasValidation?30:4;
    bool bodyEntryList=gEditorPage==2;int entryListTop=bodyEntryList?editorTop+72:editorTop+38;int entryListHeight=editorTop+editorHeight-entryListTop-validationReserve;
    move(gKvList,contentX,entryListTop,contentW,entryListHeight);
    move(gBody,contentX,editorTop+72,contentW,editorHeight-72-validationReserve);move(gBodyNone,contentX,editorTop+editorHeight/2,contentW,28);
    move(gValidation,contentX,editorTop+editorHeight-26,contentW,24);
    int responseTop=editorTop+editorHeight;move(gSummary,contentX,responseTop+4,contentW,28);
    // Keep the view selector anchored to the tabs, independent of search state.
    int responseTabsWidth=TabCtrl_GetItemCount(gResponseTabs)*SECTION_TAB_WIDTH+8;
    int responseModeX=contentX+responseTabsWidth+12;
    int findWidth=std::min(370,std::max(1,contentX+contentW-responseModeX-40-12));
    move(gResponseTabs,contentX,responseTop+34,responseTabsWidth,34);
    move(gResponseModeDivider,contentX+responseTabsWidth,responseTop+36,12,28);
    move(gResponseMode,responseModeX,responseTop+36,40,28);move(gResponseFind,contentX+contentW-28,responseTop+36,28,28);
    int responseBodyTop=responseTop+68;
    move(gResponseBody,contentX,responseBodyTop,contentW,std::max(40,height-responseBodyTop-10));
    move(gResponseHeaders,contentX,responseTop+68,contentW,std::max(40,height-responseTop-78));
    move(gResponseFindPanel,contentX+contentW-findWidth,responseTop+35,findWidth,32);
    move(gEmptyTitle,workspaceX+(workspaceW-360)/2,height/2-45,360,34);move(gEmptyHelp,workspaceX+(workspaceW-500)/2,height/2,500,28);
    if(deferred)EndDeferWindowPos(deferred);
    // Apply the narrower clip before the deferred tab measurements run, so a
    // resize cannot briefly paint or hit-test tabs over the fixed save action.
    positionRequestTabs();
    layoutResponseFindControls();updateUrlFormatting();updateResponseInsets();
    PostMessageW(gWindow,WM_UPDATE_TAB_SCROLL,0,0);
    if(gResizeSidebar){
        // Repaint the parent surface exposed by all right-side children moving.
        // WS_CLIPCHILDREN keeps this operation off the controls themselves, so
        // it clears stale background pixels without making their contents flash.
        RECT client{};GetClientRect(gWindow,&client);
        int oldWorkspaceLeft=oldTreeBounds.right+px(8);
        RECT workspaceDirty{std::min(oldWorkspaceLeft,px(workspaceX)),0,client.right,client.bottom};
        RedrawWindow(gWindow,&workspaceDirty,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_NOCHILDREN|RDW_UPDATENOW);
        // Let the edit control coalesce multiple mouse moves into one paint and
        // preserve its already-painted background while refreshing the text.
        RedrawWindow(gUrl,nullptr,nullptr,RDW_INVALIDATE|RDW_NOERASE);
        RedrawWindow(gUrlFrame,nullptr,nullptr,RDW_INVALIDATE|RDW_NOERASE);
    }
    resizeEntryColumns(gKvList,true);resizeEntryColumns(gResponseHeaders,false);
    if(freezeLists){
        SendMessageW(gKvList,WM_SETREDRAW,TRUE,0);
        SendMessageW(gResponseHeaders,WM_SETREDRAW,TRUE,0);
        RedrawWindow(gKvList,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_UPDATENOW);
        RedrawWindow(gResponseHeaders,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_UPDATENOW);
    }
    if(gResizeSidebar){
        // A TreeView owns its vertical scrollbar as non-client pixels. During
        // live resizing the common control otherwise redraws only the newly
        // exposed client strip, so old scrollbar/thumb pixels can be retained.
        // First erase the area uncovered in the parent, then repaint both the
        // complete tree client and its non-client scrollbar in the same frame.
        RECT newTreeBounds{};GetWindowRect(gTree,&newTreeBounds);MapWindowPoints(nullptr,gWindow,(POINT*)&newTreeBounds,2);
        RECT dirty{};UnionRect(&dirty,&oldTreeBounds,&newTreeBounds);
        RedrawWindow(gWindow,&dirty,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW);
        RedrawWindow(gTree,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_UPDATENOW);
    }
}

LRESULT CALLBACK windowProc(HWND h,UINT message,WPARAM w,LPARAM l) {
    switch(message) {
    case WM_CREATE: {
        gWindow=h;INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_TREEVIEW_CLASSES|ICC_TAB_CLASSES|ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&controls);
        gRichEditModule=LoadLibraryW(L"Msftedit.dll");
        gDpi=GetDpiForWindow(h);recreateFonts();
        gSidebarBrush=CreateSolidBrush(COLOR_SIDEBAR);gWhiteBrush=CreateSolidBrush(RGB(255,255,255));gAccentBrush=CreateSolidBrush(COLOR_ACCENT);gSelectedBrush=CreateSolidBrush(COLOR_SELECTED);gHoverBrush=CreateSolidBrush(COLOR_HOVER);gBorderBrush=CreateSolidBrush(COLOR_BORDER);createControls();
        TreeView_SetBkColor(gTree,COLOR_SIDEBAR);TreeView_SetTextColor(gTree,COLOR_PRIMARY);TreeView_SetLineColor(gTree,COLOR_BORDER);TreeView_SetItemHeight(gTree,px(30));
        ListView_SetBkColor(gKvList,RGB(255,255,255));ListView_SetTextColor(gKvList,COLOR_PRIMARY);ListView_SetTextBkColor(gKvList,CLR_NONE);
        ListView_SetBkColor(gResponseHeaders,RGB(255,255,255));ListView_SetTextColor(gResponseHeaders,COLOR_PRIMARY);ListView_SetTextBkColor(gResponseHeaders,CLR_NONE);
        wstring loadSource;gServices.loadData(gData,loadSource);if(gData.folders.empty()){auto folder=std::make_unique<ApiFolder>();folder->id=newId();folder->name="默认目录";gData.folders.push_back(std::move(folder));}
        gSavedContent=WorkspaceContent::from(gData);
        rebuildTree();if(!gData.folders.empty()){for(auto& folder:gData.folders)if(auto request=firstRequestIn(*folder)){openRequest(request);break;}}refreshRequestTabs();loadEditor();return 0;
    }
    case WM_SIZE:layout(LOWORD(l),HIWORD(l));return 0;
    case WM_GETMINMAXINFO:{auto info=(MINMAXINFO*)l;info->ptMinTrackSize.x=px(980);info->ptMinTrackSize.y=px(640);return 0;}
    case WM_DPICHANGED:{
        commitCellEditor();gEntryHotRow=gEntryHotColumn=-1;
        gDpi=HIWORD(w);recreateFonts();for(HWND control=GetWindow(h,GW_CHILD);control;control=GetWindow(control,GW_HWNDNEXT))applyFont(control);applyFont(gUrl,gCodeFont);applyFont(gBody,gCodeFont);applyFont(gResponseBody,gCodeFont);applyFont(gEmptyTitle,gTitleFont);for(HWND control:{gResponseFindEdit,gResponseFindPrev,gResponseFindNext,gResponseFindClose,gFindStatus})applyFont(control);updateSearchFormatting();TreeView_SetItemHeight(gTree,px(30));
        updateEntryMetrics();
        SendMessageW(gRequestTabs,TCM_SETITEMSIZE,0,MAKELPARAM(0,px(REQUEST_TAB_HEIGHT-4)));
        SendMessageW(gRequestTabs,TCM_SETPADDING,0,MAKELPARAM(px(22),px(4)));
        gEnsureSelectedRequestTabVisible=true;
        SendMessageW(gEditorTabs,TCM_SETITEMSIZE,0,MAKELPARAM(px(SECTION_TAB_WIDTH),px(30)));
        SendMessageW(gResponseTabs,TCM_SETITEMSIZE,0,MAKELPARAM(px(SECTION_TAB_WIDTH),px(30)));
        auto suggested=(RECT*)l;SetWindowPos(h,nullptr,suggested->left,suggested->top,suggested->right-suggested->left,suggested->bottom-suggested->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;
    }
    case WM_ERASEBKGND:{RECT client{};GetClientRect(h,&client);FillRect((HDC)w,&client,gWhiteBrush);RECT sidebar=client;sidebar.right=px(std::clamp(gData.sidebarWidth,180,420));FillRect((HDC)w,&sidebar,gSidebarBrush);RECT tabs{gRequestTabViewportX,0,client.right-px(10),px(REQUEST_TAB_HEIGHT)};FillRect((HDC)w,&tabs,gSidebarBrush);return 1;}
    case WM_CTLCOLOREDIT:if((HWND)l==gResponseFindEdit){SetTextColor((HDC)w,COLOR_PRIMARY);SetBkColor((HDC)w,RGB(255,255,255));return (LRESULT)gWhiteBrush;}break;
    case WM_CTLCOLORSTATIC:{HDC dc=(HDC)w;SetBkMode(dc,TRANSPARENT);if((HWND)l==gSidebarDivider){SetBkColor(dc,COLOR_BORDER);return (LRESULT)gBorderBrush;}if((HWND)l==gValidation)SetTextColor(dc,RGB(220,38,38));
        else if((HWND)l==gFindStatus)SetTextColor(dc,!gResponseFindQuery.empty()&&gResponseMatches.empty()?RGB(185,28,28):(gResponseFindWrapped?COLOR_ACCENT:COLOR_SECONDARY));
        else if((HWND)l==gSummary){auto tab=selectedTab();COLORREF color=COLOR_PRIMARY;if(tab){if(tab->sending||tab->summary.find(L"已取消")==0)color=COLOR_SECONDARY;else if(tab->summary.find(L"网络错误")==0||tab->statusCode>=400)color=RGB(185,28,28);else if(tab->statusCode>=200&&tab->statusCode<300)color=RGB(21,128,61);}SetTextColor(dc,color);}
        else SetTextColor(dc,RGB(31,41,55));return (LRESULT)gWhiteBrush;}
    case WM_DRAWITEM:drawButton((DRAWITEMSTRUCT*)l);return TRUE;
    case WM_COMMAND: {
        int id=LOWORD(w),notification=HIWORD(w);
        if(id==IDC_RESPONSE_FIND_EDIT&&notification==EN_CHANGE&&!gLoadingEditor){
            if(gImeComposing)return 0;
            gResponseFindQuery=textOf(gResponseFindEdit);refreshResponseFind(true);
            captureView();return 0;
        }
        if(id==IDC_SEARCH&&notification==EN_CHANGE){PostMessageW(h,WM_SEARCH_REFRESH,0,0);return 0;}
        if(id==IDC_URL&&notification==EN_CHANGE&&!gLoadingEditor){saveEditor();scheduleSave();}
        if(id==IDC_BODY&&notification==EN_CHANGE&&!gLoadingEditor){saveEditor();scheduleSave();}
        switch(id) {
        case IDC_RESPONSE_MODE:{
            auto tab=selectedTab();if(!tab)break;HMENU menu=CreatePopupMenu();
            AppendMenuW(menu,MF_STRING|(!tab->rawView?MF_CHECKED:0),1,L"格式化");AppendMenuW(menu,MF_STRING|(tab->rawView?MF_CHECKED:0),2,L"原始");
            RECT rect{};GetWindowRect(gResponseMode,&rect);int choice=TrackPopupMenu(menu,TPM_RETURNCMD,rect.left,rect.bottom,0,h,nullptr);DestroyMenu(menu);
            if(choice&&tab->rawView!=(choice==2)){captureView();tab->rawView=choice==2;tab->selectionStart=tab->selectionEnd=0;tab->firstVisibleLine=tab->horizontalScroll=0;tab->responseScroll={};tab->findPosition=gResponseFindPosition=wstring::npos;showResponsePage();captureView();}break;
        }
        case IDC_RESPONSE_FIND:showResponseFind(!gResponseFindVisible);break;
        case IDC_RESPONSE_FIND_PREV:if(notification==BN_CLICKED)findInResponseBody(false);break;
        case IDC_RESPONSE_FIND_NEXT:if(notification==BN_CLICKED)findInResponseBody(true);break;
        case IDC_RESPONSE_FIND_CLOSE:if(notification==BN_CLICKED)showResponseFind(false);break;
        case IDC_ADD_FOLDER:addFolder(nullptr);break;case IDC_SAVE:if(notification==BN_CLICKED)saveNow(true);break;
        case IDC_METHOD:if(notification==BN_CLICKED&&selectedTab())showMethodMenu();break;
        case IDC_SEND:sendCurrent();break;case IDC_CANCEL:cancelCurrent();break;
        case IDC_BODY_TYPE:if(notification==CBN_SELCHANGE&&!gLoadingEditor){saveEditor(false);auto tab=selectedTab();if(tab){auto& request=tab->caseSnapshot?*tab->caseSnapshot:*tab->request;int selected=(int)SendMessageW(gBodyType,CB_GETCURSEL,0,0);const char* types[]={"None","JSON","Form URL Encoded","Multipart Form Data","Raw"};if(selected>=0&&selected<5)request.bodyType=types[selected];}showEditorPage();scheduleSave();}break;
        case IDC_FORMAT:if(notification==BN_CLICKED)formatCurrentJson(false);break;
        case IDC_COMPRESS:if(notification==BN_CLICKED)formatCurrentJson(true);break;
        case IDC_SAVE_MORE:{HMENU menu=CreatePopupMenu();auto tab=selectedTab();AppendMenuW(menu,tab&&!tab->requestCase?MF_STRING:MF_GRAYED,IDM_SAVE_CASE,L"保存用例");RECT r{};GetWindowRect(gSaveMore,&r);TrackPopupMenu(menu,TPM_RIGHTBUTTON,r.left,r.bottom,0,h,nullptr);DestroyMenu(menu);break;}
        case IDM_SAVE_CASE:{auto tab=selectedTab();if(tab&&tab->request&&!tab->requestCase){wstring name;if(prompt(L"保存用例",L"请输入用例名称：",L"成功",name)){saveEditor();auto c=std::make_unique<ApiRequestCase>();c->id=newId();c->name=toUtf8(name);c->method=tab->request->method;c->url=tab->request->url;c->query=tab->request->query;c->headers=tab->request->headers;c->bodyType=tab->request->bodyType;c->body=tab->request->body;c->formFields=tab->request->formFields;c->responseSummary=toUtf8(tab->responseSummary);c->responseRaw=tab->responseRaw;c->responsePretty=tab->responsePretty;c->responseHeaders=tab->responseHeaders;tab->request->cases.push_back(std::move(c));rebuildTree();saveNow();}}break;}
        case IDM_FOLDER_ADD_REQUEST:if(gTreeSelection&&gTreeSelection->kind==NodeRef::Kind::Folder)addRequest((ApiFolder*)gTreeSelection->value);break;
        case IDM_FOLDER_ADD_CHILD:if(gTreeSelection&&gTreeSelection->kind==NodeRef::Kind::Folder)addFolder((ApiFolder*)gTreeSelection->value);break;
        case IDM_FOLDER_IMPORT:if(gTreeSelection&&gTreeSelection->kind==NodeRef::Kind::Folder)importOpenApi((ApiFolder*)gTreeSelection->value);break;
        case IDM_FOLDER_RENAME:if(gTreeSelection&&gTreeSelection->kind==NodeRef::Kind::Folder)renameFolder((ApiFolder*)gTreeSelection->value);break;
        case IDM_REQUEST_RENAME:if(gTreeSelection&&gTreeSelection->kind==NodeRef::Kind::Request)renameRequest((ApiRequest*)gTreeSelection->value);break;
        case IDM_FOLDER_DELETE:if(gTreeSelection&&gTreeSelection->kind==NodeRef::Kind::Folder)deleteFolder((ApiFolder*)gTreeSelection->value);break;
        case IDM_REQUEST_OPEN:if(gTreeSelection->kind==NodeRef::Kind::Request)openRequest((ApiRequest*)gTreeSelection->value);else if(gTreeSelection->kind==NodeRef::Kind::Case)openRequest(gTreeSelection->ownerRequest,(ApiRequestCase*)gTreeSelection->value);break;
        case IDM_REQUEST_DUPLICATE:duplicateRequest((ApiRequest*)gTreeSelection->value);break;case IDM_REQUEST_MOVE:moveRequest((ApiRequest*)gTreeSelection->value);break;case IDM_REQUEST_DELETE:deleteRequest((ApiRequest*)gTreeSelection->value);break;
        case IDM_CASE_DELETE:deleteCase(gTreeSelection->ownerRequest,(ApiRequestCase*)gTreeSelection->value);break;
        }return 0;
    }
    case WM_HSCROLL:if((HWND)l==gRequestTabScroll){scrollRequestTabs(w);return 0;}break;
    case WM_NOTIFY: {
        auto header=(LPNMHDR)l;
        if(header->idFrom==IDC_TREE&&header->code==NM_CUSTOMDRAW)return drawTreeItem((NMTVCUSTOMDRAW*)l);
        if(header->idFrom==IDC_KV_LIST&&header->code==NM_CUSTOMDRAW)return drawEntryList((NMLVCUSTOMDRAW*)l);
        if(header->idFrom==IDC_RESPONSE_HEADERS&&header->code==NM_CUSTOMDRAW){
            auto draw=(NMLVCUSTOMDRAW*)l;
            if(draw->nmcd.dwDrawStage==CDDS_PREPAINT)return CDRF_NOTIFYITEMDRAW;
            if(draw->nmcd.dwDrawStage==CDDS_ITEMPREPAINT){
                draw->nmcd.uItemState&=~(CDIS_SELECTED|CDIS_FOCUS);
                draw->clrTextBk=(draw->nmcd.uItemState&CDIS_HOT)?COLOR_HOVER:RGB(255,255,255);draw->clrText=COLOR_PRIMARY;return CDRF_NEWFONT;
            }
        }
        if(header->idFrom==IDC_TREE&&header->code==TVN_SELCHANGEDW){auto info=(NMTREEVIEWW*)l;gTreeSelection=(NodeRef*)info->itemNew.lParam;if(gTreeSelection){gSelectedFolder=gTreeSelection->ownerFolder;if(!gSelectingTree&&!gRebuildingTree&&gTreeSelection->kind==NodeRef::Kind::Request)openRequest((ApiRequest*)gTreeSelection->value);else if(!gSelectingTree&&!gRebuildingTree&&gTreeSelection->kind==NodeRef::Kind::Case)openRequest(gTreeSelection->ownerRequest,(ApiRequestCase*)gTreeSelection->value);}return 0;}
        if(header->idFrom==IDC_TREE&&header->code==TVN_BEGINDRAGW){
            auto info=(NMTREEVIEWW*)l;auto ref=(NodeRef*)info->itemNew.lParam;
            if(ref&&ref->kind==NodeRef::Kind::Request){gDragRequest=(ApiRequest*)ref->value;gDragTarget=nullptr;gDragHover=nullptr;SetCapture(h);SetCursor(LoadCursorW(nullptr,IDC_SIZEALL));}
            return 0;
        }
        if(header->idFrom==IDC_TREE&&header->code==TVN_ITEMEXPANDEDW){
            auto info=(NMTREEVIEWW*)l;auto ref=(NodeRef*)info->itemNew.lParam;
            if(ref&&ref->kind==NodeRef::Kind::Folder&&!gRebuildingTree&&!gSelectingTree){((ApiFolder*)ref->value)->expanded=info->action==TVE_EXPAND;}
            else if(ref&&ref->kind==NodeRef::Kind::Request){auto request=(ApiRequest*)ref->value;if(info->action==TVE_EXPAND)gExpandedRequests.insert(request->id);else gExpandedRequests.erase(request->id);}
            return 0;
        }
        if(header->idFrom==IDC_TREE&&header->code==NM_RCLICK){POINT point{};GetCursorPos(&point);POINT local=point;ScreenToClient(gTree,&local);TVHITTESTINFO hit{};hit.pt=local;TreeView_HitTest(gTree,&hit);if(hit.hItem){TreeView_SelectItem(gTree,hit.hItem);showTreeMenu(point);}return 0;}
        if(header->idFrom==IDC_TREE&&header->code==NM_CLICK){
            POINT point{};GetCursorPos(&point);ScreenToClient(gTree,&point);TVHITTESTINFO hit{};hit.pt=point;TreeView_HitTest(gTree,&hit);
            if(hit.hItem){TVITEMW item{};item.hItem=hit.hItem;item.mask=TVIF_PARAM;TreeView_GetItem(gTree,&item);auto ref=(NodeRef*)item.lParam;
                int contentLeft=treeItemContentLeft(hit.hItem);bool onExpander=TreeView_GetChild(gTree,hit.hItem)!=nullptr&&point.x>=contentLeft-px(18)&&point.x<contentLeft;
                if(ref&&onExpander)TreeView_Expand(gTree,hit.hItem,TVE_TOGGLE);
                else if(ref&&ref->kind==NodeRef::Kind::Folder)TreeView_Expand(gTree,hit.hItem,TVE_TOGGLE);
                else if(ref&&ref->kind==NodeRef::Kind::Request)openRequest((ApiRequest*)ref->value);
                else if(ref&&ref->kind==NodeRef::Kind::Case)openRequest(ref->ownerRequest,(ApiRequestCase*)ref->value);
            }return 0;
        }
        if(header->idFrom==IDC_REQUEST_TABS&&header->code==TCN_SELCHANGE){commitCellEditor();saveEditor();captureView();for(int& row:gSelectedEntryRows)row=-1;gSelectedTab=TabCtrl_GetCurSel(gRequestTabs);gEnsureSelectedRequestTabVisible=true;loadEditor();selectTreeTab(selectedTab());PostMessageW(gWindow,WM_UPDATE_TAB_SCROLL,0,0);return 0;}
        // Closing is intercepted before the tab control changes selection.
        if(header->idFrom==IDC_EDITOR_TABS&&header->code==TCN_SELCHANGE){commitCellEditor();saveEditor();gEditorPage=TabCtrl_GetCurSel(gEditorTabs);if(auto tab=selectedTab())tab->editorPage=gEditorPage;showEditorPage();return 0;}
        if(header->idFrom==IDC_RESPONSE_TABS&&header->code==TCN_SELCHANGE){captureView();gResponsePage=TabCtrl_GetCurSel(gResponseTabs);if(auto tab=selectedTab())tab->responsePage=gResponsePage;showResponsePage();return 0;}
        if(header->idFrom==IDC_KV_LIST&&header->code==LVN_ITEMCHANGED){auto changed=(NMLISTVIEW*)l;if(!gLoadingEntryList){gSelectedEntryRows[gEditorPage]=-1;if(changed->iItem>=0&&listCheckboxStateChanged(changed->uOldState,changed->uNewState)){saveEditor();scheduleSave();}}return 0;}
        if(header->idFrom==IDC_KV_LIST&&header->code==NM_CLICK)return 0;
        if(header->idFrom==IDC_KV_LIST&&header->code==NM_DBLCLK){auto click=(NMITEMACTIVATE*)l;if(click->iItem>=0&&click->iSubItem>0&&click->iSubItem<5)editListCell(click->iItem,click->iSubItem);return 0;}
        return 0;
    }
    case WM_EDIT_ENTRY_CELL:{auto tab=selectedTab();string bodyType=tab?(tab->caseSnapshot?tab->caseSnapshot->bodyType:tab->request->bodyType):string();bool listPage=tab&&(gEditorPage<2||(gEditorPage==2&&(bodyType=="Form URL Encoded"||bodyType=="Multipart Form Data")));int row=(int)w,column=(int)l;if(listPage&&IsWindowVisible(gKvList)&&row>=0&&row<ListView_GetItemCount(gKvList)&&column>=1&&column<=4)editListCell(row,column);return 0;}
    case WM_MOUSEMOVE:
        if(gResizeSidebar){RECT client{};GetClientRect(h,&client);gData.sidebarWidth=std::clamp(dip((int)(short)LOWORD(l)),180,std::min(420,dip((int)client.right)-660));layout((int)client.right,(int)client.bottom);SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return 0;}
        if(gResizePanels){RECT client{};GetClientRect(h,&client);int editorTop=REQUEST_EDITOR_TOP;gData.requestPanelHeight=std::clamp(dip((int)(short)HIWORD(l))-editorTop,200,std::max(200,dip((int)client.bottom)-350));layout((int)client.right,(int)client.bottom);SetCursor(LoadCursorW(nullptr,IDC_SIZENS));return 0;}
        if(gDragRequest){
            POINT point{(short)LOWORD(l),(short)HIWORD(l)};MapWindowPoints(h,gTree,&point,1);TVHITTESTINFO hit{};hit.pt=point;TreeView_HitTest(gTree,&hit);
            gDragTarget=nullptr;if(hit.hItem){TVITEMW item{};item.mask=TVIF_PARAM;item.hItem=hit.hItem;TreeView_GetItem(gTree,&item);auto ref=(NodeRef*)item.lParam;if(ref&&ref->kind==NodeRef::Kind::Folder)gDragTarget=(ApiFolder*)ref->value;else if(ref&&ref->kind==NodeRef::Kind::Request)gDragTarget=ref->ownerFolder;}
            if(hit.hItem!=gDragHover){TreeView_SelectDropTarget(gTree,hit.hItem);gDragHover=hit.hItem;}SetCursor(LoadCursorW(nullptr,gDragTarget&&gDragTarget!=findFolderForRequest(gDragRequest)?IDC_SIZEALL:IDC_NO));return 0;
        }break;
    case WM_LBUTTONUP:
        if(gResizeSidebar||gResizePanels){RECT client{};GetClientRect(h,&client);if(gResizeSidebar)gData.sidebarWidth=std::clamp(dip((int)(short)LOWORD(l)),180,std::min(420,dip((int)client.right)-660));if(gResizePanels){int editorTop=REQUEST_EDITOR_TOP;gData.requestPanelHeight=std::clamp(dip((int)(short)HIWORD(l))-editorTop,200,std::max(200,dip((int)client.bottom)-350));}gResizeSidebar=gResizePanels=false;ReleaseCapture();layout((int)client.right,(int)client.bottom);RedrawWindow(h,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);setSaveStatus(L"未保存");SetCursor(LoadCursorW(nullptr,IDC_ARROW));return 0;}
        if(gDragRequest){ReleaseCapture();TreeView_SelectDropTarget(gTree,nullptr);auto request=gDragRequest;auto target=gDragTarget;gDragRequest=nullptr;gDragTarget=nullptr;gDragHover=nullptr;if(target)moveRequestDirect(request,target);return 0;}break;
    case WM_LBUTTONDOWN: {
        POINT point{dip((short)LOWORD(l)),dip((short)HIWORD(l))};RECT client{};GetClientRect(h,&client);client.right=dip(client.right);client.bottom=dip(client.bottom);
        if(std::abs(point.x-gData.sidebarWidth)<=5){gResizeSidebar=true;SetCapture(h);SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return 0;}
        int splitterY=REQUEST_EDITOR_TOP+std::clamp(gData.requestPanelHeight,200,std::max(200,(int)client.bottom-350));
        if(point.x>gData.sidebarWidth&&std::abs(point.y-splitterY)<=5){gResizePanels=true;SetCapture(h);SetCursor(LoadCursorW(nullptr,IDC_SIZENS));return 0;}
        break;
    }
    case WM_SETCURSOR: {
        POINT point{};GetCursorPos(&point);ScreenToClient(h,&point);point.x=dip(point.x);point.y=dip(point.y);RECT client{};GetClientRect(h,&client);client.right=dip(client.right);client.bottom=dip(client.bottom);
        if(std::abs(point.x-gData.sidebarWidth)<=5){SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return TRUE;}
        int splitterY=REQUEST_EDITOR_TOP+std::clamp(gData.requestPanelHeight,200,std::max(200,(int)client.bottom-350));
        if(point.x>gData.sidebarWidth&&std::abs(point.y-splitterY)<=5){SetCursor(LoadCursorW(nullptr,IDC_SIZENS));return TRUE;}
        break;
    }
    case WM_KEYDOWN:
        if(GetKeyState(VK_CONTROL)<0&&w==VK_RETURN){sendCurrent();return 0;}if(GetKeyState(VK_CONTROL)<0&&w=='S'){saveNow(true);return 0;}
        if(GetKeyState(VK_CONTROL)<0&&w=='D'){duplicateSelectedTab();return 0;}
        if(w==VK_F2&&GetFocus()==gTree){renameSelection();return 0;}if(w==VK_DELETE&&GetFocus()==gTree&&gTreeSelection){if(gTreeSelection->kind==NodeRef::Kind::Request)deleteRequest((ApiRequest*)gTreeSelection->value);else if(gTreeSelection->kind==NodeRef::Kind::Folder)deleteFolder((ApiFolder*)gTreeSelection->value);else deleteCase(gTreeSelection->ownerRequest,(ApiRequestCase*)gTreeSelection->value);return 0;}break;
    case WM_SEARCH_REFRESH:rebuildTree();return 0;
    case WM_UPDATE_TAB_SCROLL:updateRequestTabScroll();return 0;
    case WM_TIMER:
        if(w==FIND_WRAP_TIMER){KillTimer(h,FIND_WRAP_TIMER);gResponseFindWrapped=false;updateResponseFindStatus();return 0;}
        if(w==SAVE_FEEDBACK_TIMER){KillTimer(h,SAVE_FEEDBACK_TIMER);if(!gSaveFailed&&textOf(gSave)==L"已保存"){setText(gSave,L"保存");InvalidateRect(gSave,nullptr,FALSE);}return 0;}
        if(w==DIRTY_TIMER){KillTimer(h,DIRTY_TIMER);updateDirtyStatus();return 0;}
        if(w==REQUEST_TIMER){
            bool sending=false;for(const auto& tab:gTabs)if(tab->sending){
                sending=true;auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-tab->started).count();
                tab->summary=(tab->cancel?L"正在取消 · ":L"请求中 · ")+std::to_wstring(elapsed)+L" ms";
                if(tab==selectedTab())setText(gSummary,tab->summary+(tab->oldResponse?L" · 正在显示上次响应":L""));
            }
            if(!sending)KillTimer(h,REQUEST_TIMER);return 0;
        }break;
    case WM_HTTP_DONE:{
        std::unique_ptr<HttpCompletion> completion((HttpCompletion*)w);auto tab=completion->tab;auto& result=completion->result;
        // Closed/deleted requests can still finish asynchronously; their model pointers are no longer valid.
        if(std::find(gTabs.begin(),gTabs.end(),tab)==gTabs.end())return 0;
        if(gClosingPrompt){gDeferredCompletions.emplace_back(WM_HTTP_DONE,(WPARAM)completion.release());return 0;}
        if(selectedTab()==tab)captureView();
        tab->validation.clear();tab->sending=false;
        if(result.transportSuccess){
            tab->responseRaw=std::move(result.rawBody);tab->responsePretty=std::move(result.prettyBody);tab->responseHeaders=std::move(result.headers);
            tab->statusCode=result.statusCode;tab->summary=std::to_wstring(result.statusCode)+L" "+result.statusText+L"    "+std::to_wstring(result.durationMs)+L" ms    "+formatBytes(result.sizeBytes);
            if(result.truncated)tab->summary+=L" · 响应已截断";
            tab->responseSummary=tab->summary;tab->oldResponse=false;
            tab->selectionStart=tab->selectionEnd=0;tab->firstVisibleLine=tab->horizontalScroll=0;tab->responseScroll={};tab->findPosition=wstring::npos;tab->findStatus.clear();
        }else{
            tab->summary=(result.cancelled?L"已取消    ":L"网络错误    ")+std::to_wstring(result.durationMs)+L" ms";
            tab->validation=result.cancelled?L"":result.errorMessage;tab->oldResponse=tab->responseSummary!=L"暂无响应";
        }
        if(selectedTab()==tab){
            gResponseFindPosition=tab->findPosition;setText(gFindStatus,tab->findStatus);setValidationText(tab->validation);
            setVisible(gSend,true);setVisible(gCancel,false);showResponsePage();
        }
        updateDirtyStatus();return 0;
    }
    case WM_IMPORT_DONE: {
        if(gClosingPrompt){gDeferredCompletions.emplace_back(WM_IMPORT_DONE,w);return 0;}
        std::unique_ptr<ImportDownload> download((ImportDownload*)w);gImporting=false;
        if(gImportWorker.joinable())gImportWorker.join();
        if(!download->result.transportSuccess){MessageBoxW(gWindow,download->result.errorMessage.c_str(),L"导入 OpenAPI/Swagger 失败",MB_OK|MB_ICONWARNING);return 0;}
        if(download->result.statusCode<200||download->result.statusCode>=300){wstring downloadError=L"下载 Swagger/OpenAPI 文档失败：HTTP "+std::to_wstring(download->result.statusCode)+L" "+download->result.statusText;MessageBoxW(gWindow,downloadError.c_str(),L"导入 OpenAPI/Swagger 失败",MB_OK|MB_ICONWARNING);return 0;}
        if(download->result.truncated){wstring sizeError=L"Swagger/OpenAPI 文档超过 5 MB，已取消导入。";MessageBoxW(gWindow,sizeError.c_str(),L"导入 OpenAPI/Swagger 失败",MB_OK|MB_ICONWARNING);return 0;}
        ApiFolder* target=findFolderById(download->targetId);if(!target){MessageBoxW(gWindow,L"目标目录已被删除，无法应用导入结果。",L"导入 OpenAPI/Swagger 失败",MB_OK|MB_ICONWARNING);return 0;}
        const string& importSourceUrl=download->result.finalUrl.empty()?download->sourceUrl:download->result.finalUrl;
        auto previewTarget=cloneFolderForImport(*target);OpenApiImportSummary preview;wstring previewError;
        if(!importOpenApiJson(*previewTarget,download->result.rawBody,false,preview,previewError,importSourceUrl)){MessageBoxW(gWindow,previewError.c_str(),L"导入 OpenAPI/Swagger 失败",MB_OK|MB_ICONWARNING);return 0;}
        if(preview.added+preview.overwritten+preview.skipped==0){MessageBoxW(gWindow,L"未找到可导入的接口。",L"导入 OpenAPI/Swagger",MB_OK|MB_ICONINFORMATION);return 0;}
        bool overwrite=false;if(preview.skipped>0){wstring conflictPrompt=L"发现 "+std::to_wstring(preview.skipped)+L" 个同目录同方法同 URL 的已有接口。\r\n\r\n选择“是”覆盖已有接口；选择“否”跳过已有接口；选择“取消”不导入。";int choice=MessageBoxW(gWindow,conflictPrompt.c_str(),L"导入冲突",MB_YESNOCANCEL|MB_ICONQUESTION);if(choice==IDCANCEL)return 0;overwrite=choice==IDYES;}
        OpenApiImportSummary summary;wstring error;
        if(!importOpenApiJson(*target,download->result.rawBody,overwrite,summary,error,importSourceUrl)){MessageBoxW(gWindow,error.c_str(),L"导入 OpenAPI/Swagger 失败",MB_OK|MB_ICONWARNING);return 0;}
        rebuildTree();refreshRequestTabs();loadEditor();setSaveStatus(L"未保存");
        if(!saveNow()){MessageBoxW(gWindow,L"导入数据已应用，但无法写入本地数据文件。请检查文件权限后点击“保存”重试。",L"导入 OpenAPI/Swagger 保存失败",MB_OK|MB_ICONWARNING);return 0;}
        wstring result=L"导入完成。\r\n新增："+std::to_wstring(summary.added)+L"\r\n覆盖："+std::to_wstring(summary.overwritten)+L"\r\n跳过冲突："+std::to_wstring(summary.skipped)+L"\r\n无法导入："+std::to_wstring(summary.skippedOperations);
        MessageBoxW(gWindow,result.c_str(),L"导入 OpenAPI/Swagger",MB_OK|MB_ICONINFORMATION);return 0;
    }
    case WM_CLOSE:
        commitCellEditor();saveEditor();
        if(workspaceDirty()||gSaveFailed){
            int choice=askSaveChanges(L"退出前处理未保存修改",L"接口、用例或目录有未保存修改。请选择保存全部、放弃修改或返回编辑。",true);
            if(choice==IDCANCEL)return 0;
            if(choice==IDYES&&!saveNow(true))return 0;
        }else if(!saveNow())return 0;
        gImportCancel=true;{HINTERNET handle=gImportRequest.exchange(nullptr);if(handle)WinHttpCloseHandle(handle);}if(gImportWorker.joinable())gImportWorker.join();for(auto& tab:gTabs)tab->cancelNow();for(auto& tab:gTabs)if(tab->worker.joinable())tab->worker.join();
        {MSG pending{};while(PeekMessageW(&pending,h,WM_HTTP_DONE,WM_HTTP_DONE,PM_REMOVE))delete reinterpret_cast<HttpCompletion*>(pending.wParam);
        while(PeekMessageW(&pending,h,WM_IMPORT_DONE,WM_IMPORT_DONE,PM_REMOVE))delete reinterpret_cast<ImportDownload*>(pending.wParam);}
        DestroyWindow(h);return 0;
    case WM_DESTROY:DeleteObject(gUiFont);DeleteObject(gCodeFont);DeleteObject(gTitleFont);DeleteObject(gTreeFolderFont);DeleteObject(gTreeMethodFont);DeleteObject(gEntryTypeFont);DeleteObject(gSidebarBrush);DeleteObject(gWhiteBrush);DeleteObject(gAccentBrush);DeleteObject(gSelectedBrush);DeleteObject(gHoverBrush);DeleteObject(gBorderBrush);PostQuitMessage(0);return 0;
    }return DefWindowProcW(h,message,w,l);
}
}

int runMainWindow(HINSTANCE instance,int showCommand,const MainWindowServices& services) {
    gServices=services;
    gInstance=instance;CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);gDpi=GetDpiForSystem();
    WNDCLASSW findPanelClass{};findPanelClass.hInstance=instance;findPanelClass.lpfnWndProc=responseFindPanelProc;findPanelClass.lpszClassName=L"FeatherApiResponseFind";findPanelClass.hCursor=LoadCursorW(nullptr,IDC_ARROW);findPanelClass.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&findPanelClass);
    WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=windowProc;wc.lpszClassName=L"FeatherApiWindow";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(IDI_FEATHERAPI));wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);wc.style=CS_DBLCLKS;RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"FeatherApi",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,px(1280),px(820),nullptr,nullptr,instance,nullptr);
    ShowWindow(window,showCommand);UpdateWindow(window);MSG message;while(GetMessageW(&message,nullptr,0,0)>0){
        if(message.message==WM_IME_STARTCOMPOSITION)gImeComposing=true;
        if(message.message==WM_IME_ENDCOMPOSITION)gImeComposing=false;
        bool mainActive=IsWindowEnabled(gWindow)!=FALSE&&!gClosingPrompt&&!gImeComposing;
        if(mainActive&&message.message==WM_KEYDOWN){
            bool ctrl=GetKeyState(VK_CONTROL)<0,shift=GetKeyState(VK_SHIFT)<0;HWND focus=GetFocus();
            if(!ctrl&&message.wParam==VK_RETURN){wchar_t cls[32]{};GetClassNameW(focus,cls,32);if(_wcsicmp(cls,L"Button")==0){SendMessageW(focus,BM_CLICK,0,0);continue;}}
            if(ctrl&&message.wParam=='L'&&selectedTab()){commitCellEditor();SetFocus(gUrl);SendMessageW(gUrl,EM_SETSEL,0,-1);continue;}
            if(ctrl&&message.wParam=='W'){closeTab(gSelectedTab);continue;}
            if(ctrl&&message.wParam==VK_TAB){switchRequestTab(shift);continue;}
            if(ctrl&&message.wParam=='F'&&selectedTab()){
                showResponseFind(true);continue;
            }
            if(message.wParam==VK_ESCAPE&&gResponseFindVisible&&(focus==gResponseFindEdit||IsChild(gResponseFindPanel,focus)||focus==gResponseBody)){showResponseFind(false);continue;}
            if(message.wParam==VK_TAB&&!ctrl){
                if(gCellEditor||focus==gKvList)navigateEntry(shift);else moveFocus(focus,shift);continue;
            }
        }
        if(mainActive&&message.message==WM_KEYDOWN&&GetKeyState(VK_CONTROL)<0&&message.wParam==VK_RETURN){sendCurrent();continue;}
        if(mainActive&&message.message==WM_KEYDOWN&&GetKeyState(VK_CONTROL)<0&&message.wParam=='S'){saveNow(true);continue;}
        if(mainActive&&message.message==WM_KEYDOWN&&GetKeyState(VK_CONTROL)<0&&message.wParam=='D'){duplicateSelectedTab();continue;}
        if(mainActive&&message.message==WM_KEYDOWN&&message.wParam==VK_F2&&GetFocus()==gTree){renameSelection();continue;}
        if(mainActive&&message.message==WM_KEYDOWN&&message.wParam==VK_DELETE&&GetFocus()==gTree&&gTreeSelection){
            if(gTreeSelection->kind==NodeRef::Kind::Request)deleteRequest((ApiRequest*)gTreeSelection->value);
            else if(gTreeSelection->kind==NodeRef::Kind::Folder)deleteFolder((ApiFolder*)gTreeSelection->value);
            else deleteCase(gTreeSelection->ownerRequest,(ApiRequestCase*)gTreeSelection->value);
            continue;
        }
        TranslateMessage(&message);DispatchMessageW(&message);
    }if(gRichEditModule){FreeLibrary(gRichEditModule);gRichEditModule=nullptr;}CoUninitialize();return (int)message.wParam;
}
