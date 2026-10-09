// Exercise the actual Win32 controllers against an isolated fixture. No user data is loaded or saved.
#include "ui/main_window.h"
#include "ui/ui_helpers.h"
#include "domain/interaction_state.h"
#include "application/openapi_import.h"
#include "application/request_operations.h"
#include "infrastructure/http_client.h"
#include "json/json.h"
#include "platform/encoding.h"
#include "platform/identity.h"
#include "infrastructure/storage.h"
#include <commctrl.h>
#include <filesystem>
#include <iostream>

static bool testMode=false,failSave=false;
static int closeChoice=IDCANCEL,closePrompts=0,saveCalls=0,failures=0;
static std::wstring testDataPath;
bool loadTestData(AppData&,std::wstring&);
bool saveTestData(const AppData&);
HRESULT WINAPI testTaskDialog(const TASKDIALOGCONFIG*,int*,int*,BOOL*);

// White-box controller coverage; production dependencies use the explicit service seam.
#include "ui/main_window.cpp"

bool loadTestData(AppData& data,std::wstring& source) {
    source=testDataPath;
    if(!testMode&&loadAppDataFromPath(testDataPath,data))return true;
    auto folder=std::make_unique<ApiFolder>();folder->id="folder";folder->name="交互验证（独立测试数据）";folder->expanded=true;
    auto request=std::make_unique<ApiRequest>();request->id="request";request->name="参数与响应测试";request->url="http://127.0.0.1:8765/test";
    request->query={{true,"name","中文","string","测试参数"},{true,"page","1","number","页码"}};
    auto c=std::make_unique<ApiRequestCase>();c->id="case";c->name="成功用例";c->url=request->url;
    c->responseSummary="200 OK    12 ms    50 B";c->responseRaw="{\"message\":\"中文\",\"items\":[1,2,3],\"ok\":true}";
    c->responsePretty=prettyJson(c->responseRaw);request->cases.push_back(std::move(c));folder->requests.push_back(std::move(request));
    auto second=std::make_unique<ApiRequest>();second->id="second";second->name="JSON 校验测试";second->method="POST";second->url="http://127.0.0.1:8765/test";second->bodyType="JSON";second->body="{\"ok\":true}";
    folder->requests.push_back(std::move(second));data.folders.push_back(std::move(folder));return true;
}
bool saveTestData(const AppData& data) {++saveCalls;return !failSave&&saveAppDataToPath(data,testDataPath);}
HRESULT WINAPI testTaskDialog(const TASKDIALOGCONFIG* config,int* button,int* radio,BOOL* verified) {
    if(!testMode)return TaskDialogIndirect(config,button,radio,verified);
    ++closePrompts;*button=closeChoice;return S_OK;
}
static void check(bool condition,const char* name) {
    std::cout<<(condition?"PASS  ":"FAIL  ")<<name<<"\n";if(!condition)++failures;
}
static void finish(const std::shared_ptr<TabState>& tab,bool success,bool cancelled=false,unsigned status=200) {
    auto done=new HttpCompletion();done->tab=tab;done->result.transportSuccess=success;done->result.cancelled=cancelled;
    done->result.statusCode=status;done->result.statusText=L"Test";done->result.durationMs=42;
    done->result.rawBody="{\"new\":true}";done->result.prettyBody="{\n  \"new\": true\n}";done->result.errorMessage=L"模拟连接失败";
    SendMessageW(gWindow,WM_HTTP_DONE,(WPARAM)done,0);
}
static RECT boundsInWindow(HWND control) {
    RECT bounds{};GetWindowRect(control,&bounds);MapWindowPoints(nullptr,gWindow,(POINT*)&bounds,2);return bounds;
}
static LRESULT CALLBACK observeRequestSelection(HWND h,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
    if(message==WM_NOTIFY){auto header=(NMHDR*)l;if(header->hwndFrom==gRequestTabs&&header->code==TCN_SELCHANGE)++*(int*)data;}
    return DefSubclassProc(h,message,w,l);
}
static void runRequestTabTests(ApiRequest* request,ApiRequest* second) {
    const UINT originalDpi=gDpi;const int originalSidebar=gData.sidebarWidth;RECT originalWindow{};GetWindowRect(gWindow,&originalWindow);
    updateRequestTabScroll();
    RECT client{},first{};GetClientRect(gRequestTabs,&client);TabCtrl_GetItemRect(gRequestTabs,0,&first);
    HDC screen=GetDC(gRequestTabs),buffer=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,client.right,client.bottom);auto oldBitmap=SelectObject(buffer,bitmap);
    PatBlt(buffer,0,0,client.right,client.bottom,BLACKNESS);
    SendMessageW(gRequestTabs,WM_PRINTCLIENT,(WPARAM)buffer,PRF_CLIENT);
    int tintedPixels=0;
    for(int y=0;y<client.bottom;++y)for(int x=first.left;x<first.right;++x){
        COLORREF color=GetPixel(buffer,x,y);
        if(GetRValue(color)>160&&GetGValue(color)>GetRValue(color)+8&&GetGValue(color)>GetBValue(color)+5)++tintedPixels;
    }
    check(tintedPixels>px(10)*px(5),"request tab WM_PRINTCLIENT paints the GET method badge instead of the native strip");
    SelectObject(buffer,oldBitmap);DeleteObject(bitmap);DeleteDC(buffer);ReleaseDC(gRequestTabs,screen);

    openRequest(second);openRequest(request);updateRequestTabScroll();auto active=selectedTab();
    RECT clean{},dirty{};TabCtrl_GetItemRect(gRequestTabs,0,&clean);
    const wstring originalUrl=textOf(gUrl);setText(gUrl,L"http://127.0.0.1/keep-current-draft");updateDirtyStatus();updateRequestTabScroll();
    TabCtrl_GetItemRect(gRequestTabs,0,&dirty);
    check(clean.right-clean.left==dirty.right-dirty.left,"dirty status does not shift request tab widths");
    int selectionChanges=0,prompts=closePrompts;const auto count=gTabs.size();
    SetWindowSubclass(gWindow,observeRequestSelection,71,(DWORD_PTR)&selectionChanges);
    RECT close=requestTabCloseRect(gRequestTabs,1);
    SendMessageW(gRequestTabs,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM((close.left+close.right)/2,(close.top+close.bottom)/2));
    SendMessageW(gRequestTabs,WM_LBUTTONUP,0,MAKELPARAM((close.left+close.right)/2,(close.top+close.bottom)/2));
    RemoveWindowSubclass(gWindow,observeRequestSelection,71);
    check(gTabs.size()+1==count&&selectedTab()==active&&selectionChanges==0&&closePrompts==prompts&&
          textOf(gUrl)==L"http://127.0.0.1/keep-current-draft"&&tabDirty(*active),
          "closing an inactive request tab never selects it or changes the current draft");
    setText(gUrl,originalUrl);updateDirtyStatus();

    // Local requests exercise overflow without adding anything to the saved workspace.
    std::vector<std::unique_ptr<ApiRequest>> overflow;
    for(int i=0;i<8;++i){
        auto extra=std::make_unique<ApiRequest>();extra->id="tab-overflow-"+std::to_string(i);
        extra->name="请求标签滚动测试 "+std::to_string(i);extra->url="http://127.0.0.1/test";
        openRequest(extra.get());overflow.push_back(std::move(extra));
    }
    int baselineHeight=0;
    for(UINT dpi:{96u,144u,192u}){
        gData.sidebarWidth=420;RECT proposed{0,0,MulDiv(980,(int)dpi,96),MulDiv(640,(int)dpi,96)};
        SendMessageW(gWindow,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),(LPARAM)&proposed);
        updateRequestTabScroll();
        RECT tabs=boundsInWindow(gRequestTabs),scroll=boundsInWindow(gRequestTabScroll),last{};
        TabCtrl_GetItemRect(gRequestTabs,TabCtrl_GetItemCount(gRequestTabs)-1,&last);
        if(dpi==96)baselineHeight=tabs.bottom-tabs.top;
        check(tabs.bottom-tabs.top==MulDiv(baselineHeight,(int)dpi,96)&&tabs.bottom<=scroll.top,
              "request strip scales at 100/150/200 percent without overlapping its scrollbar");
        check((GetWindowLongPtrW(gRequestTabScroll,GWL_STYLE)&WS_VISIBLE)&&last.left>=gRequestTabScrollOffset&&
              last.right<=gRequestTabScrollOffset+gRequestTabViewportWidth,
              "selecting an overflow request brings its entire tab into view at every DPI");
        const RECT fixedSave=boundsInWindow(gSave),fixedMore=boundsInWindow(gSaveMore);
        auto saveAreaIsProtected=[&](){
            HRGN region=CreateRectRgn(0,0,0,0);const int regionKind=GetWindowRgn(gRequestTabs,region);
            RECT clipped{};GetRgnBox(region,&clipped);DeleteObject(region);
            RECT currentTabs=boundsInWindow(gRequestTabs);OffsetRect(&clipped,currentTabs.left,currentTabs.top);
            RECT currentSave=boundsInWindow(gSave),currentMore=boundsInWindow(gSaveMore),currentScroll=boundsInWindow(gRequestTabScroll);
            return regionKind!=ERROR&&regionKind!=NULLREGION&&clipped.right<=fixedSave.left&&currentScroll.right<=fixedSave.left&&
                EqualRect(&fixedSave,&currentSave)&&EqualRect(&fixedMore,&currentMore);
        };
        bool saveProtected=saveAreaIsProtected();
        auto selected=selectedTab();SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(gRequestTabScroll,SB_CTL,&info);
        const int pages=(info.nMax-info.nMin+1)/std::max(1,(int)info.nPage)+2;
        for(int i=0;i<pages;++i)SendMessageW(gWindow,WM_HSCROLL,SB_PAGELEFT,(LPARAM)gRequestTabScroll);
        const bool reachedStart=gRequestTabScrollOffset==0;saveProtected=saveAreaIsProtected()&&saveProtected;
        for(int i=0;i<pages;++i)SendMessageW(gWindow,WM_HSCROLL,SB_PAGERIGHT,(LPARAM)gRequestTabScroll);
        check(reachedStart&&gRequestTabScrollOffset+gRequestTabViewportWidth>=last.right&&selectedTab()==selected,
              "horizontal scrolling reaches both ends without switching the active request");
        check(saveAreaIsProtected()&&saveProtected,
              "the scrolled tab region and scrollbar stay clear of the fixed save controls at both ends");
    }
    while(gTabs.size()>1)closeTab((int)gTabs.size()-1,false);
    gData.sidebarWidth=originalSidebar;SendMessageW(gWindow,WM_DPICHANGED,MAKEWPARAM(originalDpi,originalDpi),(LPARAM)&originalWindow);
    openRequest(request);updateRequestTabScroll();updateDirtyStatus();
}
static void runEntryTableTests(ApiRequest* request) {
    const auto originalQuery=request->query;const auto originalContent=RequestContent::from(*request);
    const UINT originalDpi=gDpi;RECT originalWindow{};GetWindowRect(gWindow,&originalWindow);
    HWND header=ListView_GetHeader(gKvList);RECT first{},checkboxColumn{};
    ListView_GetItemRect(gKvList,0,&first,LVIR_BOUNDS);Header_GetItemRect(header,0,&checkboxColumn);
    MapWindowPoints(header,gKvList,(POINT*)&checkboxColumn,2);
    const LPARAM click=MAKELPARAM((checkboxColumn.left+checkboxColumn.right)/2,(first.top+first.bottom)/2);
    const bool originallyChecked=ListView_GetCheckState(gKvList,0)!=FALSE;
    SendMessageW(gKvList,WM_LBUTTONDOWN,MK_LBUTTON,click);
    const bool checkedAfterDown=ListView_GetCheckState(gKvList,0)!=FALSE;
    SendMessageW(gKvList,WM_LBUTTONUP,0,click);
    check(checkedAfterDown!=originallyChecked&&(ListView_GetCheckState(gKvList,0)!=FALSE)==checkedAfterDown&&
          request->query[0].enabled==checkedAfterDown,"a checkbox mouse click toggles exactly once and updates the request");
    SendMessageW(gKvList,WM_LBUTTONDOWN,MK_LBUTTON,click);SendMessageW(gKvList,WM_LBUTTONUP,0,click);

    // Exercise the real paint path before reading the list back into the request.
    RECT client{};GetClientRect(gKvList,&client);HDC screen=GetDC(gKvList),buffer=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,client.right,client.bottom);auto oldBitmap=SelectObject(buffer,bitmap);
    SendMessageW(gKvList,WM_PRINTCLIENT,(WPARAM)buffer,PRF_CLIENT);
    SelectObject(buffer,oldBitmap);DeleteObject(bitmap);DeleteDC(buffer);ReleaseDC(gKvList,screen);
    saveEditor();int trailing=ListView_GetItemCount(gKvList)-1;bool empty=true;
    for(int column=1;column<=4;++column)empty=empty&&listCellText(gKvList,trailing,column).empty();
    check(empty&&RequestContent::from(*request)==originalContent,
          "painting the add-parameter hint keeps placeholder text out of request data");
    ListView_SetItemState(gKvList,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
    ListView_SetItemState(gKvList,trailing,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    SendMessageW(gKvList,WM_KEYDOWN,VK_SPACE,0);
    check(gCellEditor&&gEditRow==trailing&&gEditColumn==1&&ListView_GetCheckState(gKvList,trailing),
          "Space on the add-parameter row starts editing without silently disabling the new entry");
    setText(gCellEditor,L"added_from_trailing_row");commitCellEditor();
    trailing=ListView_GetItemCount(gKvList)-1;empty=true;
    for(int column=1;column<=4;++column)empty=empty&&listCellText(gKvList,trailing,column).empty();
    check(request->query.size()==originalQuery.size()+1&&request->query.back().key=="added_from_trailing_row"&&
          ListView_GetItemCount(gKvList)==(int)request->query.size()+1&&empty,
          "editing the trailing parameter row appends one entry and preserves exactly one empty row");

    for(UINT dpi:{96u,144u,192u}){
        RECT proposed{0,0,MulDiv(1280,(int)dpi,96),MulDiv(820,(int)dpi,96)};
        SendMessageW(gWindow,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),(LPARAM)&proposed);
        RECT row{},heading=boundsInWindow(header);ListView_GetItemRect(gKvList,1,&row,LVIR_BOUNDS);
        check(std::abs((int)(row.bottom-row.top)-px(32))<=1&&std::abs((int)(heading.bottom-heading.top)-px(32))<=1,
              "native parameter rows and header remain 32 DIPs at 100/150/200 percent");
        RECT value{};ListView_GetSubItemRect(gKvList,1,2,LVIR_BOUNDS,&value);
        LVHITTESTINFO hit{};hit.pt={(value.left+value.right)/2,(value.top+value.bottom)/2};ListView_SubItemHitTest(gKvList,&hit);
        check(hit.iItem==1&&hit.iSubItem==2,"native parameter hit testing matches the displayed cell at every DPI");
        editListCell(1,3,true);RECT type{};ListView_GetSubItemRect(gKvList,1,3,LVIR_BOUNDS,&type);
        MapWindowPoints(gKvList,gWindow,(POINT*)&type,2);RECT editor=boundsInWindow(gCellEditor);
        check(gCellEditor&&gEditRow==1&&gEditColumn==3&&editor.top>=type.top&&editor.bottom<=type.bottom&&
              editor.left>=type.left&&editor.right<=type.right,"the type dropdown fits inside its native parameter cell at every DPI");
        commitCellEditor(false);
    }

    request->query.clear();
    for(int i=0;i<40;++i)request->query.push_back({true,"scroll-row-"+std::to_string(i),"before","string",""});
    fillEntryList(gKvList,request->query);editListCell(0,2,true);setText(gCellEditor,L"committed_before_scroll");
    HWND editing=gCellEditor;const int top=ListView_GetTopIndex(gKvList);
    SendMessageW(gKvList,WM_VSCROLL,SB_LINEDOWN,0);
    check(!gCellEditor&&!IsWindow(editing)&&request->query[0].value=="committed_before_scroll"&&ListView_GetTopIndex(gKvList)>top,
          "scrolling commits the current parameter edit and removes its overlay before moving rows");

    commitCellEditor(false);request->query=originalQuery;fillEntryList(gKvList,request->query);
    SendMessageW(gWindow,WM_DPICHANGED,MAKEWPARAM(originalDpi,originalDpi),(LPARAM)&originalWindow);
    gFocusedEntryColumn=1;ListView_SetItemState(gKvList,-1,0,LVIS_SELECTED|LVIS_FOCUSED);updateDirtyStatus();
    check(RequestContent::from(*request)==originalContent,"parameter table checks restore the original request fixture");
}
static void runUrlParamSyncTests(ApiRequest* request,ApiRequest* second) {
    const RequestContent original=RequestContent::from(*request);
    const int previousSaveCalls=saveCalls;
    ApiRequest legacy;legacy.url="https://example.invalid/legacy?from_url=a%2Fb#anchor";
    legacy.query={{true,"from_grid","2"}};normalizeStoredUrlQuery(legacy);normalizeStoredUrlQuery(legacy);
    check(legacy.url=="https://example.invalid/legacy#anchor"&&legacy.query.size()==2&&
          buildRequestUrl({"GET",legacy.url,legacy.query})==
              "https://example.invalid/legacy?from_url=a%2Fb&from_grid=2#anchor",
          "opening an older URL merges raw and table query entries once");
    AppData legacyWorkspace;auto legacyFolder=std::make_unique<ApiFolder>();
    auto legacyRequest=std::make_unique<ApiRequest>();legacyRequest->url="https://example.invalid/old?request=1";
    auto legacyCase=std::make_unique<ApiRequestCase>();legacyCase->url="https://example.invalid/old?case=2";
    legacyRequest->cases.push_back(std::move(legacyCase));legacyFolder->requests.push_back(std::move(legacyRequest));
    legacyWorkspace.folders.push_back(std::move(legacyFolder));normalizeWorkspaceUrlQueries(legacyWorkspace);
    const WorkspaceContent normalizedBaseline=WorkspaceContent::from(legacyWorkspace);
    normalizeWorkspaceUrlQueries(legacyWorkspace);
    check(legacyWorkspace.folders[0]->requests[0]->query.size()==1&&
          legacyWorkspace.folders[0]->requests[0]->cases[0]->query.size()==1&&
          WorkspaceContent::from(legacyWorkspace)==normalizedBaseline,
          "startup normalizes older requests and cases once before establishing the saved baseline");
    ApiRequest repeatedKey;repeatedKey.url="https://example.invalid/duplicates";
    repeatedKey.query={{true,"tag","a","","first note"},{true,"tag","b","","second note"}};
    int previousPage=gEditorPage;gEditorPage=1;
    acceptEditedUrl(L"https://example.invalid/duplicates?tag=b&tag=b",repeatedKey);
    gEditorPage=previousPage;
    check(repeatedKey.query.size()==2&&repeatedKey.query[0].description=="first note"&&
          repeatedKey.query[1].description=="second note",
          "editing duplicate parameter values keeps each row's description in place");
    const wstring pasted=L"https://example.invalid/items?tag=one&tag=two&empty=&flag&word=%E4%B8%AD%E6%96%87&space=a+b#part";
    setText(gUrl,pasted);
    check(textOf(gUrl)==pasted&&request->url=="https://example.invalid/items#part"&&request->query.size()==6&&
          request->query[0].key=="tag"&&request->query[1].key=="tag"&&request->query[2].value.empty()&&
          request->query[3].key=="flag"&&request->query[4].value=="中文"&&request->query[5].value=="a b"&&
          ListView_GetItemCount(gKvList)==7,
          "pasting a URL exposes duplicate, empty, bare and encoded query entries in Params");
    check(buildRequestUrl(snapshotCurrent())==
          "https://example.invalid/items?tag=one&tag=two&empty=&flag=&word=%E4%B8%AD%E6%96%87&space=a%20b#part",
          "the request sends parsed entries once and keeps the URL fragment (bare flags normalize to flag=)");
    ListView_SetCheckState(gKvList,1,FALSE);
    check(!request->query[1].enabled&&textOf(gUrl).find(L"tag=two")==wstring::npos&&
          buildRequestUrl(snapshotCurrent()).find("tag=two")==string::npos,
          "disabling a pasted parameter removes it from the displayed and sent URL");
    editListCell(0,2,true);setText(gCellEditor,L"changed value");commitCellEditor();
    check(request->query[0].value=="changed value"&&textOf(gUrl).find(L"tag=changed%20value")!=wstring::npos&&
          buildRequestUrl(snapshotCurrent()).find("tag=changed%20value")!=string::npos,
          "editing a parameter updates the displayed and sent URL with encoding");
    request->query[0].description="keep enabled note";
    request->query[1].description="keep disabled note";loadEditor();
    wstring editedVisible=textOf(gUrl);
    size_t changedAt=editedVisible.find(L"tag=changed%20value");
    check(changedAt!=wstring::npos,"URL fixture contains the enabled parameter to edit");
    if(changedAt!=wstring::npos)editedVisible.replace(changedAt,wcslen(L"tag=changed%20value"),L"tag=updated");
    setText(gUrl,editedVisible);
    check(request->query.size()==6&&request->query[0].value=="updated"&&
          request->query[0].description=="keep enabled note"&&!request->query[1].enabled&&
          request->query[1].description=="keep disabled note"&&
          ListView_GetItemCount(gKvList)==7,
          "editing a visible URL parameter keeps disabled Params rows and row descriptions");
    setText(gUrl,L"https://example.invalid/items?tag=updated&");
    check(request->query.size()==6&&!request->query[1].enabled&&
          buildRequestUrl(snapshotCurrent())=="https://example.invalid/items?tag=updated&",
          "an unfinished query keeps Params rows without appending them to the sent draft");
    openRequest(second);openRequest(request);
    check(textOf(gUrl)==L"https://example.invalid/items?tag=updated&"&&request->query.size()==6,
          "switching tabs preserves the unfinished query and its last complete Params rows");
    setText(gUrl,editedVisible);
    check(request->query.size()==6&&!request->query[1].enabled&&
          request->query[1].description=="keep disabled note",
          "finishing a query edit restores its disabled Params rows");
    const string finalUrl=buildRequestUrl(snapshotCurrent());
    openRequest(second);openRequest(request);
    check(buildRequestUrl(snapshotCurrent())==finalUrl&&request->query.size()==6&&
          textOf(gUrl)==toWide(finalUrl),"switching request tabs keeps Params synchronized without duplicate query entries");
    check(saveNow()&&request->url=="https://example.invalid/items#part"&&request->query.size()==6,
          "saving stores the address and query separately without duplicating the URL query");
    for(int row=0;row<6;++row)ListView_SetCheckState(gKvList,row,FALSE);
    setText(gUrl,L"https://example.invalid/renamed#part");
    bool allDisabled=request->query.size()==6;
    for(const auto& entry:request->query)allDisabled=allDisabled&&!entry.enabled;
    check(allDisabled&&request->url=="https://example.invalid/renamed#part"&&
          buildRequestUrl(snapshotCurrent())=="https://example.invalid/renamed#part",
          "editing the path keeps query rows when every parameter is disabled");
    ListView_SetCheckState(gKvList,0,TRUE);
    setText(gUrl,L"https://example.invalid/renamed#part");
    bool keptDisabled=request->query.size()==5;
    for(const auto& entry:request->query)keptDisabled=keptDisabled&&!entry.enabled;
    check(keptDisabled&&buildRequestUrl(snapshotCurrent())=="https://example.invalid/renamed#part",
          "removing the visible query clears enabled parameters but keeps disabled rows");
    setText(gUrl,L"https://example.invalid/draft?");openRequest(second);openRequest(request);
    check(textOf(gUrl)==L"https://example.invalid/draft?"&&request->query.size()==5,
          "switching tabs preserves an unfinished question mark in the URL");
    setText(gUrl,L"https://example.invalid/draft?term=%");openRequest(second);openRequest(request);
    check(textOf(gUrl)==L"https://example.invalid/draft?term=%"&&request->query.size()==5,
          "switching tabs preserves an unfinished percent escape in the URL");
    setText(gUrl,L"https://example.invalid/draft?term=%20");
    check(request->url=="https://example.invalid/draft"&&request->query.size()==6&&request->query.back().value==" "&&
          buildRequestUrl(snapshotCurrent())=="https://example.invalid/draft?term=%20",
          "finishing a percent escape creates an editable Params row, keeps disabled rows, and sends it once");
    ApiRequestCase* requestCase=request->cases.front().get();const CaseContent originalCase=CaseContent::from(*requestCase);
    openRequest(request,requestCase);setText(gUrl,L"https://example.invalid/case?case=one&case=two#result");
    check(selectedTab()->caseSnapshot->url=="https://example.invalid/case#result"&&
          selectedTab()->caseSnapshot->query.size()==2&&
          buildRequestUrl(snapshotCurrent())=="https://example.invalid/case?case=one&case=two#result",
          "a case URL populates its own Params rows and sends duplicate names once");
    openRequest(second);openRequest(request,requestCase);
    check(textOf(gUrl)==L"https://example.invalid/case?case=one&case=two#result"&&
          selectedTab()->caseSnapshot->query.size()==2&&saveNow()&&
          requestCase->url=="https://example.invalid/case#result"&&requestCase->query.size()==2,
          "switching and saving a case keeps its URL and Params synchronized");
    originalCase.apply(*requestCase);originalCase.request.apply(*selectedTab()->caseSnapshot);loadEditor();
    openRequest(request);
    original.apply(*request);loadEditor();saveNow();saveCalls=previousSaveCalls;
    check(RequestContent::from(*request)==original,"URL/Params checks restore the original request fixture");
}
static void runRequestToolbarTests(ApiRequest* request) {
    const auto originalContent=RequestContent::from(*request);const wstring originalUrl=textOf(gUrl),originalMethod=textOf(gMethod);
    const UINT originalDpi=gDpi;const int originalSidebar=gData.sidebarWidth;RECT originalWindow{};GetWindowRect(gWindow,&originalWindow);
    const wstring address=L"https://example.invalid/接口?name=中文&query="+wstring(180,L'x');
    SendMessageW(gUrl,EM_SETSEL,0,-1);SendMessageW(gUrl,EM_REPLACESEL,TRUE,(LPARAM)address.c_str());
    check(textOf(gUrl)==address&&request->url=="https://example.invalid/接口"&&request->query.size()==2&&
          request->query[0].value=="中文"&&buildRequestUrl(snapshotCurrent()).find("name=%E4%B8%AD%E6%96%87")!=string::npos,
          "native URL editing keeps Unicode and long queries visible and sends each parameter once");
    SendMessageW(gUrl,WM_CHAR,VK_RETURN,0);
    check(textOf(gUrl)==address,"Enter in the URL field never inserts a newline");
    SetFocus(gUrl);SetFocus(gMethod);
    check(textOf(gUrl)==address&&request->url=="https://example.invalid/接口"&&request->query.size()==2,
          "focusing the method selector preserves URL text and request address");
    selectRequestMethod(L"PATCH");
    check(textOf(gUrl)==address&&request->url=="https://example.invalid/接口"&&request->method=="PATCH",
          "switching request method preserves the visible URL and request address");
    RECT frameBounds{},urlBounds{};GetClientRect(gUrlFrame,&frameBounds);GetWindowRect(gUrl,&urlBounds);
    MapWindowPoints(nullptr,gUrlFrame,(POINT*)&urlBounds,2);
    HDC screen=GetDC(gUrlFrame),buffer=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,frameBounds.right,frameBounds.bottom);
    auto oldBitmap=SelectObject(buffer,bitmap);FillRect(buffer,&frameBounds,gWhiteBrush);
    int urlCenterX=(urlBounds.left+urlBounds.right)/2,urlCenterY=(urlBounds.top+urlBounds.bottom)/2;
    SetPixelV(buffer,urlCenterX,urlCenterY,RGB(255,0,255));
    DRAWITEMSTRUCT frameDraw{};frameDraw.CtlID=IDC_URL_FRAME;frameDraw.hwndItem=gUrlFrame;
    frameDraw.hDC=buffer;frameDraw.rcItem=frameBounds;drawRequestControl(&frameDraw);
    check(GetPixel(buffer,urlCenterX,urlCenterY)==RGB(255,0,255),
          "URL frame repaint does not cover pixels inside the URL editor");
    SelectObject(buffer,oldBitmap);DeleteObject(bitmap);DeleteDC(buffer);ReleaseDC(gUrlFrame,screen);
    const DWORD selectedStart=10,selectedEnd=24;SendMessageW(gUrl,EM_SETSEL,selectedStart,selectedEnd);
    for(UINT dpi:{96u,144u,192u}){
        gData.sidebarWidth=420;RECT proposed{0,0,MulDiv(980,(int)dpi,96),MulDiv(640,(int)dpi,96)};
        SendMessageW(gWindow,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),(LPARAM)&proposed);
        RECT client{};GetClientRect(gWindow,&client);std::vector<RECT> actions;
        for(HWND control:{gMethod,gUrl,gSave,gSaveMore,gSend,gEditorTabs})actions.push_back(boundsInWindow(control));
        bool usable=true;
        for(size_t i=0;i<actions.size();++i){
            const RECT& rect=actions[i];usable=usable&&rect.left>=0&&rect.right<=client.right&&rect.top>=0&&
                rect.bottom<=client.bottom&&rect.right>rect.left&&rect.bottom>rect.top;
            for(size_t j=i+1;j<actions.size();++j){RECT overlap{};usable=usable&&!IntersectRect(&overlap,&rect,&actions[j]);}
        }
        RECT frame=boundsInWindow(gUrlFrame),url=boundsInWindow(gUrl),send=boundsInWindow(gSend),cancel=boundsInWindow(gCancel);
        check(usable&&url.left>=frame.left&&url.right<=frame.right&&url.top>=frame.top&&url.bottom<=frame.bottom&&EqualRect(&send,&cancel),
              "request controls stay usable at minimum width and send/cancel share one slot at every DPI");
        RECT save=boundsInWindow(gSave),more=boundsInWindow(gSaveMore),tabs=boundsInWindow(gRequestTabs);
        check(send.left>=frame.right&&send.left-frame.right<=px(16)&&save.bottom<=frame.top&&more.bottom<=frame.top&&
              save.top>=tabs.top&&save.bottom<=tabs.bottom&&more.top>=tabs.top&&more.bottom<=tabs.bottom,
              "the URL uses the space before Send while save actions stay in the top request-tab strip at every DPI");
        RECT editClient{},format{};GetClientRect(gUrl,&editClient);SendMessageW(gUrl,EM_GETRECT,0,(LPARAM)&format);
        check(format.left>=0&&format.right<=editClient.right&&format.top>=0&&format.bottom<=editClient.bottom&&
              format.right>format.left&&format.bottom-format.top>=fontTextHeight(gUrl,gCodeFont),
              "the native URL text rectangle fits its editor without clipping at every DPI");
        DWORD start=0,end=0;SendMessageW(gUrl,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
        check(textOf(gUrl)==address&&request->url=="https://example.invalid/接口"&&start==selectedStart&&end==selectedEnd,
              "toolbar relayout preserves the native URL text and selection");
    }
    gData.sidebarWidth=originalSidebar;SendMessageW(gWindow,WM_DPICHANGED,MAKEWPARAM(originalDpi,originalDpi),(LPARAM)&originalWindow);
    selectRequestMethod(originalMethod.c_str());originalContent.apply(*request);loadEditor();updateDirtyStatus();
    check(RequestContent::from(*request)==originalContent&&textOf(gUrl)==originalUrl,
          "request toolbar checks restore the original request fixture");
}
static CHARFORMAT2W responseFormatAt(size_t position) {
    DWORD start=0,end=0;POINT scroll{};SendMessageW(gResponseBody,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);SendMessageW(gResponseBody,EM_GETSCROLLPOS,0,(LPARAM)&scroll);
    SendMessageW(gResponseBody,WM_SETREDRAW,FALSE,0);CHARFORMAT2W format{};format.cbSize=sizeof(format);
    SendMessageW(gResponseBody,EM_SETSEL,(WPARAM)position,(LPARAM)(position+1));
    SendMessageW(gResponseBody,EM_GETCHARFORMAT,SCF_SELECTION,(LPARAM)&format);
    SendMessageW(gResponseBody,EM_SETSEL,start,end);SendMessageW(gResponseBody,EM_SETSCROLLPOS,0,(LPARAM)&scroll);SendMessageW(gResponseBody,WM_SETREDRAW,TRUE,0);return format;
}
static bool hasAutomaticBackground(const CHARFORMAT2W& format) {
    return !(format.dwMask&CFM_BACKCOLOR)||(format.dwEffects&CFE_AUTOBACKCOLOR);
}
static void responseSelection(DWORD& start,DWORD& end) {
    SendMessageW(gResponseBody,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
}
static bool responseMatchIsVisible(size_t position,size_t length) {
    RECT viewport{};SendMessageW(gResponseBody,EM_GETRECT,0,(LPARAM)&viewport);
    POINTL first{},last{};
    SendMessageW(gResponseBody,EM_POSFROMCHAR,(WPARAM)&first,(LPARAM)position);
    SendMessageW(gResponseBody,EM_POSFROMCHAR,(WPARAM)&last,(LPARAM)(position+length));
    return first.x>=viewport.left&&first.x<viewport.right&&first.y>=viewport.top&&first.y<viewport.bottom&&
           last.x>=viewport.left&&last.x<viewport.right&&last.y>=viewport.top&&last.y<viewport.bottom;
}
static void pressResponseFindEnter(bool previous) {
    BYTE originalKeys[256]{},keys[256]{};GetKeyboardState(originalKeys);for(int i=0;i<256;++i)keys[i]=originalKeys[i];
    if(previous)keys[VK_SHIFT]|=0x80;else keys[VK_SHIFT]&=0x7f;
    SetKeyboardState(keys);SendMessageW(gResponseFindEdit,WM_KEYDOWN,VK_RETURN,0);SetKeyboardState(originalKeys);
}
static void runLargeResponseFindTests(const std::shared_ptr<TabState>& tab) {
    if(gResponseFindVisible)SendMessageW(gResponseFindClose,BM_CLICK,0,0);
    const wstring longQuery(300,L'x');tab->rawView=true;tab->responseRaw=toUtf8(L"\""+longQuery+L"\"");tab->responsePretty=tab->responseRaw;showResponsePage();
    SendMessageW(gResponseBody,EM_SETSEL,1,301);SetFocus(gResponseBody);
    BYTE originalKeys[256]{},ctrlKeys[256]{};GetKeyboardState(originalKeys);for(int i=0;i<256;++i)ctrlKeys[i]=originalKeys[i];ctrlKeys[VK_CONTROL]|=0x80;
    SetKeyboardState(ctrlKeys);SendMessageW(gResponseBody,WM_KEYDOWN,'F',0);SetKeyboardState(originalKeys);
    DWORD start=0,end=0;responseSelection(start,end);
    check(textOf(gResponseFindEdit)==longQuery&&textOf(gFindStatus)==L"1 / 1"&&start==1&&end==301,
          "Ctrl+F seeds and finds an arbitrary long single-line response selection");
    SendMessageW(gResponseFindClose,BM_CLICK,0,0);

    std::string raw="[";for(int i=0;i<180;++i){if(i)raw+=",";raw+="\"scroll-hit\"";}raw+="]";
    tab->rawView=false;tab->responseRaw=raw;tab->responsePretty=prettyJson(raw);showResponsePage();const auto rendered=responseText();
    const size_t first=rendered.find(L"scroll-hit"),last=rendered.rfind(L"scroll-hit"),previous=rendered.rfind(L"scroll-hit",last-1);
    SetFocus(gResponseFind);SendMessageW(gResponseFind,BM_CLICK,0,0);SetFocus(gResponseFindEdit);setText(gResponseFindEdit,L"scroll-hit");responseSelection(start,end);
    check(textOf(gFindStatus)==L"1 / 180"&&start==first&&end==first+10,"long response search counts every match while selecting the first");
    const auto offscreen=responseFormatAt(last);check(hasAutomaticBackground(offscreen),"an offscreen response match is not eagerly formatted");
    check(!responseMatchIsVisible(last,10),"the final response match starts outside the viewport");
    pressResponseFindEnter(true);SendMessageW(gResponseBody,WM_PAINT,0,0);
    check(responseMatchIsVisible(last,10),"previous search scrolls an offscreen final match into view while the query keeps focus");
    check(GetFocus()==gResponseFindEdit,"scrolling to a response match keeps keyboard focus in the query");
    pressResponseFindEnter(false);SendMessageW(gResponseBody,WM_PAINT,0,0);
    check(responseMatchIsVisible(first,10),"next search wraps and scrolls back to the first match");
    SendMessageW(gResponseBody,EM_SETSEL,(WPARAM)first,(LPARAM)(first+10));
    const int lineCount=(int)SendMessageW(gResponseBody,EM_GETLINECOUNT,0,0);SendMessageW(gResponseBody,EM_LINESCROLL,0,lineCount-5);
    // The fixture window is hidden, so RedrawWindow does not dispatch WM_PAINT.
    // Exercise its real paint handler explicitly after moving the viewport.
    SendMessageW(gResponseBody,WM_PAINT,0,0);
    const auto visibleLast=responseFormatAt(last);check(!hasAutomaticBackground(visibleLast),"scrolling and repainting applies the match background when an offscreen result becomes visible");
    SendMessageW(gResponseBody,EM_SETSEL,(WPARAM)first,(LPARAM)(first+10));pressResponseFindEnter(true);SendMessageW(gResponseBody,WM_PAINT,0,0);responseSelection(start,end);
    const auto currentLast=responseFormatAt(last),visiblePrevious=responseFormatAt(previous);
    check(start==last&&end==last+10&&textOf(gFindStatus).find(L"180 / 180")!=wstring::npos,"previous navigation wraps to the final match in a long response");
    check(!hasAutomaticBackground(currentLast)&&!hasAutomaticBackground(visiblePrevious)&&currentLast.crBackColor!=visiblePrevious.crBackColor,
          "the newly current visible match is stronger than neighboring visible matches");
    SendMessageW(gResponseFindClose,BM_CLICK,0,0);
    tab->selectionStart=tab->selectionEnd=0;tab->responseScroll={};tab->firstVisibleLine=tab->horizontalScroll=0;

    raw="raw-hit\n";for(int i=0;i<180;++i)raw+=std::string(600,'x')+"\n";raw+="raw-hit tail-only";
    tab->rawView=true;tab->responseRaw=raw;tab->responsePretty=raw;showResponsePage();
    const auto rawRendered=responseText();const size_t rawLast=rawRendered.rfind(L"raw-hit"),tail=rawRendered.find(L"tail-only");
    SetFocus(gResponseFind);SendMessageW(gResponseFind,BM_CLICK,0,0);SetFocus(gResponseFindEdit);
    check(!responseMatchIsVisible(tail,9),"the unique raw response match starts below the viewport beyond 64K characters");
    setText(gResponseFindEdit,L"tail-only");SendMessageW(gResponseBody,WM_PAINT,0,0);
    check(responseMatchIsVisible(tail,9)&&GetFocus()==gResponseFindEdit,"typing a query scrolls its offscreen first result into view without taking focus");
    setText(gResponseFindEdit,L"raw-hit");SendMessageW(gResponseBody,WM_PAINT,0,0);
    check(responseMatchIsVisible(0,7),"changing a query scrolls from the bottom back to its first result");
    SetFocus(gResponseFindNext);SendMessageW(gResponseFindNext,BM_CLICK,0,0);SendMessageW(gResponseBody,WM_PAINT,0,0);
    check(responseMatchIsVisible(rawLast,7)&&GetFocus()==gResponseFindNext,"the next button reveals a distant match without taking keyboard focus");
    SendMessageW(gResponseFindClose,BM_CLICK,0,0);
    tab->selectionStart=tab->selectionEnd=0;tab->responseScroll={};tab->firstVisibleLine=tab->horizontalScroll=0;

    tab->responseRaw="raw-hit "+std::string(600,'x')+" raw-hit tail-only";tab->responsePretty=tab->responseRaw;showResponsePage();
    const auto wideRendered=responseText();const size_t wideLast=wideRendered.rfind(L"raw-hit"),wideTail=wideRendered.find(L"tail-only");
    SetFocus(gResponseFind);SendMessageW(gResponseFind,BM_CLICK,0,0);SetFocus(gResponseFindEdit);setText(gResponseFindEdit,L"raw-hit");
    check(!responseMatchIsVisible(wideLast,7),"a long raw line has a match outside the horizontal viewport");
    pressResponseFindEnter(false);SendMessageW(gResponseBody,WM_PAINT,0,0);
    check(responseMatchIsVisible(wideLast,7),"next search horizontally scrolls a long raw line to its current match");
    pressResponseFindEnter(true);SendMessageW(gResponseBody,WM_PAINT,0,0);
    check(responseMatchIsVisible(0,7),"previous search scrolls a long raw line back to its first match");
    setText(gResponseFindEdit,L"tail-only");SendMessageW(gResponseBody,WM_PAINT,0,0);
    check(responseMatchIsVisible(wideTail,9),"typing a query horizontally reveals its offscreen first result");
    SendMessageW(gResponseFindClose,BM_CLICK,0,0);
    tab->selectionStart=tab->selectionEnd=0;tab->responseScroll={};tab->firstVisibleLine=tab->horizontalScroll=0;
}
static void runResponseFindTests(const std::shared_ptr<TabState>& tab,ApiRequest* request,ApiRequestCase* requestCase,ApiRequest* otherRequest) {
    tab->rawView=false;tab->responseRaw="{\"match\":\"match\",\"other\":\"match\"}";tab->responsePretty=prettyJson(tab->responseRaw);showResponsePage();
    const auto rendered=responseText();
    const size_t first=rendered.find(L"match"),second=rendered.find(L"match",first+1),third=rendered.find(L"match",second+1),other=rendered.find(L"other");
    check(first!=wstring::npos&&second!=wstring::npos&&third!=wstring::npos&&rendered.find(L"match",third+1)==wstring::npos,"response-search fixture has three hand-checked matches");

    if(gResponseFindVisible)SendMessageW(gResponseFindClose,BM_CLICK,0,0);
    SendMessageW(gResponseBody,EM_SETSEL,(WPARAM)other,(LPARAM)(other+5));SetFocus(gResponseBody);
    BYTE originalCtrlKeys[256]{},ctrlKeys[256]{};GetKeyboardState(originalCtrlKeys);std::copy(std::begin(originalCtrlKeys),std::end(originalCtrlKeys),std::begin(ctrlKeys));ctrlKeys[VK_CONTROL]|=0x80;
    SetKeyboardState(ctrlKeys);SendMessageW(gResponseBody,WM_KEYDOWN,'F',0);SetKeyboardState(originalCtrlKeys);
    DWORD start=0,end=0;responseSelection(start,end);
    check(textOf(gResponseFindEdit)==L"other"&&textOf(gFindStatus)==L"1 / 1"&&start==other&&end==other+5,"Ctrl+F seeds the focused response selection and immediately finds it");
    SendMessageW(gResponseFindClose,BM_CLICK,0,0);
    responseSelection(start,end);check(start==other&&end==other+5,"closing response search retains the selected text for copying");

    SetFocus(gResponseFind);SendMessageW(gResponseFind,BM_CLICK,0,0);SetFocus(gResponseFindEdit);setText(gResponseFindEdit,L"match");
    responseSelection(start,end);
    check(start==first&&end==first+5&&textOf(gFindStatus)==L"1 / 3","typing a visible response query immediately selects the first match and reports 1 / N");
    check(IsWindowEnabled(gResponseFindPrev)&&IsWindowEnabled(gResponseFindNext),"response-search arrows enable when matches exist");

    auto firstFormat=responseFormatAt(first),secondFormat=responseFormatAt(second),thirdFormat=responseFormatAt(third);
    const auto pale=[](COLORREF color){return GetRValue(color)+GetGValue(color)+GetBValue(color)>450;};
    check((firstFormat.dwMask&CFM_BACKCOLOR)&&(secondFormat.dwMask&CFM_BACKCOLOR)&&(thirdFormat.dwMask&CFM_BACKCOLOR)&&
          !(firstFormat.dwEffects&CFE_AUTOBACKCOLOR)&&!(secondFormat.dwEffects&CFE_AUTOBACKCOLOR)&&!(thirdFormat.dwEffects&CFE_AUTOBACKCOLOR)&&
          pale(firstFormat.crBackColor)&&pale(secondFormat.crBackColor)&&pale(thirdFormat.crBackColor)&&
          secondFormat.crBackColor==thirdFormat.crBackColor&&firstFormat.crBackColor!=secondFormat.crBackColor,
          "all visible matches have a pale background and the current match is stronger");
    check(firstFormat.crTextColor==RGB(29,78,160)&&(firstFormat.dwEffects&CFE_BOLD)&&
          secondFormat.crTextColor==RGB(21,128,61)&&!(secondFormat.dwEffects&CFE_BOLD),
          "response-search backgrounds preserve JSON foreground colors and bold keys");
    SendMessageW(gResponseBody,EM_SETSEL,(WPARAM)first,(LPARAM)(first+5));

    pressResponseFindEnter(false);responseSelection(start,end);
    check(start==second&&end==second+5&&textOf(gFindStatus)==L"2 / 3","Enter advances to the next response match");
    pressResponseFindEnter(true);responseSelection(start,end);
    check(start==first&&end==first+5&&textOf(gFindStatus).find(L"1 / 3")!=wstring::npos,"Shift+Enter moves to the previous response match");
    pressResponseFindEnter(true);responseSelection(start,end);
    check(start==third&&end==third+5,"previous-match navigation wraps without replacing the compact count with prose");

    openRequest(otherRequest);openRequest(request,requestCase);responseSelection(start,end);
    check(gResponseFindVisible&&textOf(gResponseFindEdit)==L"match"&&textOf(gFindStatus).find(L"3 / 3")!=wstring::npos&&start==third&&end==third+5,
          "each response tab restores its search query and current match");

    SetFocus(gResponseFindEdit);setText(gResponseFindEdit,L"missing");responseSelection(start,end);
    check(textOf(gFindStatus)==L"未找到"&&!IsWindowEnabled(gResponseFindPrev)&&!IsWindowEnabled(gResponseFindNext)&&start==third&&end==third+5,
          "a missing response query disables navigation without moving the response selection");
    setText(gResponseFindEdit,L"");responseSelection(start,end);
    check(!IsWindowEnabled(gResponseFindPrev)&&!IsWindowEnabled(gResponseFindNext)&&start==third&&end==third+5,
          "an empty response query disables navigation without moving the response selection");

    setText(gResponseFindEdit,L"match");SendMessageW(gResponseFindEdit,WM_KEYDOWN,VK_RETURN,0);responseSelection(start,end);
    const DWORD selectedStart=start,selectedEnd=end;
    SendMessageW(gResponseBody,EM_SETSEL,selectedStart,selectedEnd);SendMessageW(gResponseFindClose,BM_CLICK,0,0);responseSelection(start,end);
    const auto closedFirst=responseFormatAt(first),closedSecond=responseFormatAt(second),closedThird=responseFormatAt(third);
    check(start==selectedStart&&end==selectedEnd,"closing response search preserves the current response selection");
    check(hasAutomaticBackground(closedFirst)&&hasAutomaticBackground(closedSecond)&&hasAutomaticBackground(closedThird),
          "closing response search removes match backgrounds");
    check(closedSecond.crTextColor==RGB(21,128,61)&&!(closedSecond.dwEffects&CFE_BOLD),"closing search keeps JSON syntax formatting intact");

    for(UINT dpi:{96u,144u,192u}){
        // Ask for the widest supported sidebar: the layout clamps this request
        // to the maximum that still leaves the workspace usable at 980 DIPs.
        gData.sidebarWidth=420;
        RECT proposed{0,0,MulDiv(980,(int)dpi,96),MulDiv(640,(int)dpi,96)};
        SendMessageW(gWindow,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),(LPARAM)&proposed);
        RECT closedBody=boundsInWindow(gResponseBody),toolbar=boundsInWindow(gResponseTabs),closedMode=boundsInWindow(gResponseMode),headersItem{};
        TabCtrl_GetItemRect(gResponseTabs,1,&headersItem);MapWindowPoints(gResponseTabs,gWindow,(POINT*)&headersItem,2);
        const int modeGap=closedMode.left-headersItem.right;
        check(modeGap>=0&&modeGap<=(closedMode.right-closedMode.left)/2,
              "response mode follows the Body and Headers items with a compact gap at 100/150/200 percent");
        SetFocus(gResponseMode);SendMessageW(gResponseFind,BM_CLICK,0,0);
        RECT openBody=boundsInWindow(gResponseBody),openMode=boundsInWindow(gResponseMode),find=boundsInWindow(gResponseFindPanel),client{};GetClientRect(gWindow,&client);
        check(EqualRect(&closedBody,&openBody),"opening inline response search does not shift body geometry at 100/150/200 percent");
        check(EqualRect(&closedMode,&openMode),"opening inline response search does not move the response mode at 100/150/200 percent");
        check(find.left>=openMode.right&&find.right>find.left,
              "inline response search opens to the right without overlapping the response mode at 100/150/200 percent");
        check(find.top>=toolbar.top&&find.bottom<=toolbar.bottom&&find.left>=0&&find.right<=client.right&&find.right>find.left,
              "minimum-window response search stays compact inside the existing toolbar at 100/150/200 percent");
        check(textOf(gResponseFindEdit)==L"match","opening search outside the response body does not seed from an old body selection");
        SendMessageW(gResponseFindClose,BM_CLICK,0,0);
    }
}
static void runSidebarSearchTests() {
    auto& root=*gData.folders[0];
    auto nested=std::make_unique<ApiFolder>();nested->id="search-folder";nested->name="Billing archive";nested->expanded=false;
    auto request=std::make_unique<ApiRequest>();request->id="search-request";request->name="Billing upload";
    request->url="https://example.invalid/search-by-url";
    auto match=std::make_unique<ApiRequestCase>();match->id="search-case";match->name="Refund hidden-only";
    auto other=std::make_unique<ApiRequestCase>();other->id="search-other-case";other->name="Unrelated scenario";
    auto nestedPtr=nested.get();auto requestPtr=request.get();
    auto matchPtr=match.get();auto otherPtr=other.get();
    request->cases.push_back(std::move(match));request->cases.push_back(std::move(other));
    nested->requests.push_back(std::move(request));root.children.push_back(std::move(nested));rebuildTree();
    HTREEITEM initialRequestItem=findTreeValue(NodeRef::Kind::Request,requestPtr);
    check(initialRequestItem&&!treeItemIsRevealed(initialRequestItem),
          "sidebar fixture request starts hidden inside a collapsed folder");

    setText(gSearch,L"Refund hidden-only");rebuildTree();
    HTREEITEM folderItem=findTreeValue(NodeRef::Kind::Folder,nestedPtr);
    HTREEITEM requestItem=findTreeValue(NodeRef::Kind::Request,requestPtr);
    HTREEITEM caseItem=findTreeValue(NodeRef::Kind::Case,matchPtr);
    check(folderItem&&requestItem&&caseItem&&treeItemIsRevealed(caseItem)&&
          (TreeView_GetItemState(gTree,folderItem,TVIS_EXPANDED)&TVIS_EXPANDED)&&
          (TreeView_GetItemState(gTree,requestItem,TVIS_EXPANDED)&TVIS_EXPANDED),
          "searching a case name reveals its folder, request, and case");
    check(!findTreeValue(NodeRef::Kind::Case,otherPtr),"case search excludes unrelated cases");
    syncVisibleFolderExpansionState();
    check(!nestedPtr->expanded&&!gExpandedRequests.count(requestPtr->id),
          "search expansion does not overwrite the saved folder or request state");
    TreeView_Expand(gTree,folderItem,TVE_COLLAPSE);TreeView_Expand(gTree,folderItem,TVE_EXPAND);
    TreeView_Expand(gTree,requestItem,TVE_COLLAPSE);TreeView_Expand(gTree,requestItem,TVE_EXPAND);
    check(!nestedPtr->expanded&&!gExpandedRequests.count(requestPtr->id),
          "toggling search results does not replace the user's unfiltered expansion state");

    setText(gSearch,L"Billing upload");rebuildTree();
    requestItem=findTreeValue(NodeRef::Kind::Request,requestPtr);
    check(requestItem&&treeItemIsRevealed(requestItem),"request name search reveals a nested request");
    setText(gSearch,L"search-by-url");rebuildTree();
    requestItem=findTreeValue(NodeRef::Kind::Request,requestPtr);
    check(requestItem&&treeItemIsRevealed(requestItem),"URL search reveals a nested request");

    setText(gSearch,L"");rebuildTree();
    folderItem=findTreeValue(NodeRef::Kind::Folder,nestedPtr);
    requestItem=findTreeValue(NodeRef::Kind::Request,requestPtr);
    check(folderItem&&requestItem&&!treeItemIsRevealed(requestItem)&&!nestedPtr->expanded&&
          !gExpandedRequests.count(requestPtr->id),
          "clearing search restores the user's collapsed tree state");
    root.children.pop_back();rebuildTree();
}
struct TreeRedrawProbe {bool redrawEnabled=true;int resets=0,unbatchedResets=0;};
static LRESULT CALLBACK observeTreeRedraw(HWND h,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
    auto& probe=*(TreeRedrawProbe*)data;
    if(message==WM_SETREDRAW)probe.redrawEnabled=w!=FALSE;
    if(message==TVM_DELETEITEM&&(HTREEITEM)l==TVI_ROOT){++probe.resets;if(probe.redrawEnabled)++probe.unbatchedResets;}
    return DefSubclassProc(h,message,w,l);
}
static void runTreeExpansionTests(ApiRequest* request,ApiRequest* second) {
    auto folder=gData.folders[0].get();const int saves=saveCalls;
    auto folderItem=[&](){return findTreeValue(NodeRef::Kind::Folder,folder);};
    auto expanded=[](HTREEITEM item){return (TreeView_GetItemState(gTree,item,TVIS_EXPANDED)&TVIS_EXPANDED)!=0;};
    TreeRedrawProbe probe;SetWindowSubclass(gTree,observeTreeRedraw,72,(DWORD_PTR)&probe);

    openRequest(second);openRequest(request);auto active=selectedTab();
    // Match the sidebar's toggle message after initial tree construction has expanded the folder once.
    TreeView_Expand(gTree,folderItem(),TVE_TOGGLE);
    check(!expanded(folderItem())&&!folder->expanded,"collapsing an already-expanded folder immediately records its actual state");
    closeTab(1);
    check(gTabs.size()==1&&selectedTab()==active&&!expanded(folderItem()),
          "closing an inactive tab preserves the collapsed folder and active request");

    TreeView_Expand(gTree,folderItem(),TVE_EXPAND);openRequest(second);
    TreeView_Expand(gTree,folderItem(),TVE_COLLAPSE);closeTab(gSelectedTab);
    check(gTabs.size()==1&&selectedTab()==active&&!expanded(folderItem()),
          "closing the active tab does not expand the remaining request's folder");

    TreeView_Expand(gTree,folderItem(),TVE_EXPAND);
    auto requestItem=findTreeRequest(request);TreeView_Expand(gTree,requestItem,TVE_EXPAND);
    openRequest(request,request->cases[0].get());TreeView_Expand(gTree,requestItem,TVE_TOGGLE);
    check(!expanded(requestItem)&&!gExpandedRequests.count(request->id),
          "collapsing a request records its hidden case list after the first expansion");
    closeTab(0);
    check(gTabs.size()==1&&selectedTab()->requestCase==request->cases[0].get()&&!expanded(findTreeRequest(request)),
          "closing a request tab does not reopen the remaining case tab's collapsed parent");

    TreeView_Expand(gTree,folderItem(),TVE_COLLAPSE);closeTabRange(gSelectedTab,IDM_TAB_CLOSE_ALL);
    check(gTabs.empty()&&!expanded(folderItem())&&saveCalls==saves,
          "closing all tabs preserves folder collapse without saving the workspace");
    check(probe.resets==0,"closing clean active, inactive, and all tabs never clears the sidebar tree");
    RemoveWindowSubclass(gTree,observeTreeRedraw,72);
    TreeView_Expand(gTree,folderItem(),TVE_EXPAND);openRequest(request);
}
static void runTreeDiscardRedrawTests(ApiRequest* request) {
    const int previousChoice=closeChoice;closeChoice=IDNO;
    const auto saved=RequestContent::from(*request);
    setText(gUrl,L"http://127.0.0.1/discard-tree-filter");saveEditor();
    setText(gSearch,L"discard-tree-filter");rebuildTree();
    check(findTreeRequest(request)!=nullptr,"edited request URL appears in sidebar search before discard");
    TreeRedrawProbe probe;SetWindowSubclass(gTree,observeTreeRedraw,72,(DWORD_PTR)&probe);
    closeTab(gSelectedTab);
    check(gTabs.empty()&&RequestContent::from(*request)==saved&&!findTreeRequest(request),
          "discarding request changes updates sidebar search membership");
    check(probe.resets==1&&probe.unbatchedResets==0&&probe.redrawEnabled,
          "discard rebuilds the sidebar once with intermediate painting disabled");

    setText(gSearch,L"");rebuildTree();openRequest(request);
    auto fresh=std::make_unique<ApiRequest>();fresh->id="discard-tree-new";fresh->name="Discard tree fixture";
    auto freshPtr=fresh.get();gData.folders[0]->requests.push_back(std::move(fresh));rebuildTree();openRequest(freshPtr);
    probe={};closeTab(gSelectedTab);
    check(gTabs.size()==1&&selectedTab()->request==request&&gData.folders[0]->requests.size()==2&&
          TreeView_GetCount(gTree)==4&&probe.resets==1&&probe.unbatchedResets==0&&probe.redrawEnabled,
          "discarding a new request removes its tree row in one batched refresh");
    RemoveWindowSubclass(gTree,observeTreeRedraw,72);closeChoice=previousChoice;
}
static void checkFlatEditorBorder(HWND editor) {
    RECT bounds{},client{};GetWindowRect(editor,&bounds);GetClientRect(editor,&client);
    POINT origin{};ClientToScreen(editor,&origin);
    const int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
    HDC screen=GetDC(editor),buffer=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height);auto previous=SelectObject(buffer,bitmap);
    PatBlt(buffer,0,0,width,height,BLACKNESS);
    SendMessageW(editor,WM_PRINT,(WPARAM)buffer,PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);
    const COLORREF border=GetFocus()==editor?COLOR_ACCENT:COLOR_BORDER;
    bool even=true;
    for(int x=0;x<width;++x)even=even&&GetPixel(buffer,x,0)==border&&GetPixel(buffer,x,height-1)==border;
    for(int y=0;y<height;++y)even=even&&GetPixel(buffer,0,y)==border&&GetPixel(buffer,width-1,y)==border;
    check(even,"code editor paints all four border edges with one uniform color");
    check(!(GetWindowLongPtrW(editor,GWL_STYLE)&WS_BORDER)&&
          !(GetWindowLongPtrW(editor,GWL_EXSTYLE)&(WS_EX_CLIENTEDGE|WS_EX_STATICEDGE))&&
          origin.x-bounds.left==1&&origin.y-bounds.top==1,
          "code editor reserves a single pixel for its custom border without a native inset edge");
    SelectObject(buffer,previous);DeleteObject(bitmap);DeleteDC(buffer);ReleaseDC(editor,screen);
}
static void runBodyEditorTests(ApiRequest* request,ApiRequest* second) {
    const auto original=RequestContent::from(*second);const UINT originalDpi=gDpi;
    RECT originalWindow{};GetWindowRect(gWindow,&originalWindow);
    openRequest(second);selectedTab()->editorPage=2;
    second->body="{\r\n  \"message\": \"中文\",\r\n  \"count\": 12,\r\n  \"ok\": true,\r\n  \"empty\": null\r\n}";
    loadEditor();const auto initialText=textOf(gBody);const auto initialContent=RequestContent::from(*second);
    wchar_t bodyClass[64]{},responseClass[64]{};GetClassNameW(gBody,bodyClass,64);GetClassNameW(gResponseBody,responseClass,64);
    check(gRichEditModule&&wstring(bodyClass)==responseClass,"request and response bodies use the same native code editor");
    auto formatAt=[&](const wstring& needle){
        const auto text=codeEditorText(gBody);const auto start=text.find(needle);
        SendMessageW(gBody,EM_SETSEL,start,start+needle.size());CHARFORMAT2W format{};format.cbSize=sizeof(format);
        SendMessageW(gBody,EM_GETCHARFORMAT,SCF_SELECTION,(LPARAM)&format);return format;
    };
    auto key=formatAt(L"message"),value=formatAt(L"中文"),number=formatAt(L"12"),boolean=formatAt(L"true");
    check(key.crTextColor==RGB(29,78,160)&&(key.dwEffects&CFE_BOLD)&&value.crTextColor==RGB(21,128,61)&&
          !(value.dwEffects&CFE_BOLD)&&number.crTextColor==RGB(180,83,9)&&boolean.crTextColor==RGB(126,34,206),
          "request JSON keys, strings, numbers, and literals match the response palette");
    SendMessageW(gBody,EM_SETSEL,3,11);CHARRANGE before{},after{};POINT scrollBefore{},scrollAfter{};
    SendMessageW(gBody,EM_EXGETSEL,0,(LPARAM)&before);SendMessageW(gBody,EM_GETSCROLLPOS,0,(LPARAM)&scrollBefore);
    SendMessageW(gBody,EM_SETMODIFY,FALSE,0);updateBodyAppearance();
    SendMessageW(gBody,EM_EXGETSEL,0,(LPARAM)&after);SendMessageW(gBody,EM_GETSCROLLPOS,0,(LPARAM)&scrollAfter);
    check(before.cpMin==after.cpMin&&before.cpMax==after.cpMax&&scrollBefore.x==scrollAfter.x&&scrollBefore.y==scrollAfter.y&&
          !SendMessageW(gBody,EM_GETMODIFY,0,0)&&!SendMessageW(gBody,EM_CANUNDO,0,0)&&textOf(gBody)==initialText&&
          RequestContent::from(*second)==initialContent,"syntax styling preserves selection, scroll, text, dirty state, and empty undo history");

    SendMessageW(gBody,EM_SETSEL,(WPARAM)-1,-1);SendMessageW(gBody,EM_REPLACESEL,TRUE,(LPARAM)L" ");
    const auto edited=textOf(gBody);SendMessageW(gWindow,WM_TIMER,BODY_STYLE_TIMER,0);
    SendMessageW(gBody,EM_UNDO,0,0);updateBodyAppearance();
    check(textOf(gBody)==initialText&&SendMessageW(gBody,EM_CANREDO,0,0),"JSON highlighting leaves the text edit undoable and preserves redo");
    SendMessageW(gBody,EM_REDO,0,0);updateBodyAppearance();
    check(textOf(gBody)==edited&&second->body==toUtf8(edited),"redo restores body text and the request model after highlighting");

    setText(gBody,L"{\"kk\":1,\"message\":\"中文\"}");const auto compact=textOf(gBody);
    formatCurrentJson(false);check(textOf(gBody).find(L'\n')!=wstring::npos,"request JSON still formats as editable multiline text");
    SendMessageW(gBody,EM_UNDO,0,0);updateBodyAppearance();
    check(textOf(gBody)==compact,"formatting JSON is one undoable text edit");
    setText(gBody,L"{\r\n  \"中文\": tru");SendMessageW(gWindow,WM_TIMER,BODY_STYLE_TIMER,0);
    check(second->body==toUtf8(textOf(gBody)),"incomplete Unicode JSON remains editable and synchronized");
    gBodyImeComposing=true;SendMessageW(gBody,EM_SETSEL,(WPARAM)-1,-1);SendMessageW(gBody,EM_REPLACESEL,TRUE,(LPARAM)L"e");
    SendMessageW(gBody,EM_EXGETSEL,0,(LPARAM)&before);updateBodyAppearance();SendMessageW(gBody,EM_EXGETSEL,0,(LPARAM)&after);
    check(gBodyImeComposing&&before.cpMin==after.cpMin&&before.cpMax==after.cpMax,"body styling waits until IME composition finishes");
    SendMessageW(gBody,WM_IME_ENDCOMPOSITION,0,0);SendMessageW(gWindow,WM_TIMER,BODY_STYLE_TIMER,0);
    check(!gBodyImeComposing,"body styling resumes after IME composition");

    SendMessageW(gBodyType,CB_SETCURSEL,4,0);SendMessageW(gWindow,WM_COMMAND,MAKEWPARAM(IDC_BODY_TYPE,CBN_SELCHANGE),(LPARAM)gBodyType);
    auto rawFormat=formatAt(L"中文");check(rawFormat.crTextColor==COLOR_PRIMARY&&!(rawFormat.dwEffects&CFE_BOLD),"Raw body mode clears JSON syntax coloring");
    const wstring raw=L"{\\rtf1\\ansi literal payload}\r\n中文";setText(gBody,raw);updateBodyAppearance();saveEditor();
    check(textOf(gBody)==raw&&second->body==toUtf8(raw),"Raw payloads containing an RTF header stay literal text");
    openRequest(request);openRequest(second);
    check(textOf(gBody)==raw&&!SendMessageW(gBody,EM_CANUNDO,0,0),"switching tabs preserves raw body data and does not mix undo histories");
    const wstring large(2*1024*1024+16,L'x');setText(gBody,large);updateBodyAppearance();
    check(textOf(gBody)==large&&SendMessageW(gBody,EM_GETLIMITTEXT,0,0)==2*1024*1024,
          "loading an existing large body preserves all text and restores the interactive input limit");
    const wstring payload(2*1024*1024-16,L'x');setText(gBody,L"{\"payload\":\""+payload+L"\"}");
    const wstring largeCompact=textOf(gBody);formatCurrentJson(false);
    check(textOf(gBody)==L"{\r\n  \"payload\": \""+payload+L"\"\r\n}"&&SendMessageW(gBody,EM_GETLIMITTEXT,0,0)==2*1024*1024,
          "formatting near the input limit keeps the complete expanded JSON body");
    SendMessageW(gBody,EM_UNDO,0,0);check(textOf(gBody)==largeCompact,"large JSON formatting remains undoable without truncation");
    setText(gBody,raw);
    for(UINT dpi:{96u,144u,192u}){
        RECT proposed{0,0,MulDiv(1100,(int)dpi,96),MulDiv(760,(int)dpi,96)};
        SendMessageW(gWindow,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),(LPARAM)&proposed);
        RECT bodyRect{},responseRect{};SendMessageW(gBody,EM_GETRECT,0,(LPARAM)&bodyRect);SendMessageW(gResponseBody,EM_GETRECT,0,(LPARAM)&responseRect);
        check(bodyRect.left==responseRect.left&&bodyRect.top==responseRect.top&&bodyRect.left>=px(10)&&bodyRect.top>=px(7),
              "request and response editor insets match at 100/150/200 percent DPI");
        SetFocus(gUrl);checkFlatEditorBorder(gBody);checkFlatEditorBorder(gResponseBody);
        SetFocus(gBody);checkFlatEditorBorder(gBody);
    }
    original.apply(*second);loadEditor();gDpi=originalDpi;SendMessageW(gWindow,WM_DPICHANGED,MAKEWPARAM(originalDpi,originalDpi),(LPARAM)&originalWindow);
    openRequest(request);
}
static void runControllerTests() {
    auto request=gData.folders[0]->requests[0].get();auto second=gData.folders[0]->requests[1].get();auto c=request->cases[0].get();
    check(!workspaceDirty(),"fixture starts clean");
    runSidebarSearchTests();
    runTreeExpansionTests(request,second);
    runTreeDiscardRedrawTests(request);
    runRequestTabTests(request,second);
    runEntryTableTests(request);
    runUrlParamSyncTests(request,second);
    runRequestToolbarTests(request);
    runBodyEditorTests(request,second);
    const wstring savedVisibleUrl=textOf(gUrl);
    const size_t queryStart=savedVisibleUrl.find(L'?');
    const wstring visibleQuery=queryStart==wstring::npos?L"":savedVisibleUrl.substr(queryStart);
    setText(gUrl,L"http://127.0.0.1/edited"+visibleQuery);updateDirtyStatus();check(tabDirty(*selectedTab())&&gSaveStatus==L"未保存","URL edits immediately mark the tab and workspace dirty");
    TreeView_Expand(gTree,TreeView_GetRoot(gTree),TVE_COLLAPSE);check(saveCalls==0,"expanding or collapsing a folder never saves pending request edits");
    TreeView_Expand(gTree,TreeView_GetRoot(gTree),TVE_EXPAND);
    setText(gUrl,savedVisibleUrl);updateDirtyStatus();check(!workspaceDirty(),"restoring original URL clears dirty state");

    editListCell(0,1,true);setText(gCellEditor,L"changed");navigateEntry(false);
    check(gEditRow==0&&gEditColumn==2&&request->query[0].key=="changed","Tab commits and enters next column");
    setText(gCellEditor,L"discard-me");SendMessageW(gCellEditor,WM_KEYDOWN,VK_ESCAPE,0);
    check(request->query[0].value=="中文"&&!gCellEditor,"Escape restores the current cell only");
    editListCell(0,2,true);navigateEntry(false);check(gEditColumn==3,"Tab reaches type dropdown");navigateEntry(false);check(gEditColumn==4,"Tab leaves type dropdown for description");
    navigateEntry(false);check(gEditRow==1&&gEditColumn==1,"Tab continues onto the next row");navigateEntry(true);check(gEditRow==0&&gEditColumn==4,"Shift+Tab crosses rows backwards");commitCellEditor();
    openRequest(second);openRequest(request);check(request->query[0].key=="changed","switching tabs preserves committed edits");
    check(saveNow()&&!workspaceDirty(),"save all advances the baseline");
    openRequest(request,c);setText(gUrl,L"http://127.0.0.1/case-only");
    check(tabDirty(*selectedTab()),"case URL edits are tracked before switching tabs");
    check(saveNow()&&c->url=="http://127.0.0.1/case-only"&&request->url!=c->url&&c->id=="case","save updates existing case without changing its parent or identity");
    const auto saved=gSavedContent;setText(gUrl,L"http://127.0.0.1/retry");failSave=true;
    check(!saveNow()&&gSaveFailed&&gSavedContent==saved&&tabDirty(*selectedTab()),"failed save preserves the last successful baseline and dirty draft");
    SendMessageW(gWindow,WM_TIMER,SAVE_FEEDBACK_TIMER,0);
    check(textOf(gSave)==L"保存失败","a late success-feedback timer cannot hide the current save failure");
    AppData disk;check(loadAppDataFromPath(testDataPath,disk)&&disk.folders[0]->requests[0]->cases[0]->url=="http://127.0.0.1/case-only","failed save leaves the file unchanged");
    failSave=false;check(saveNow(true)&&!workspaceDirty()&&!gSaveFailed,"retry saves the retained case draft");
    check(textOf(gSave)==L"已保存","a successful retry confirms saving on the button");
    SendMessageW(gWindow,WM_TIMER,SAVE_FEEDBACK_TIMER,0);
    check(textOf(gSave)==L"保存","success feedback returns to the save action after its timer");

    auto tab=selectedTab();
    check(gRichEditModule!=nullptr,"response Rich Edit is available");
    tab->responseRaw="{\"first\":1,\"user_name\":\"hello-world 中文\",\"ok\":true,\"empty\":null}";tab->responsePretty=prettyJson(tab->responseRaw);showResponsePage();
    auto rendered=responseText();auto key=rendered.find(L"user_name");
    SendMessageW(gResponseBody,EM_SETSEL,(WPARAM)key,(LPARAM)(key+9));
    CHARFORMAT2W keyFormat{};keyFormat.cbSize=sizeof(keyFormat);SendMessageW(gResponseBody,EM_GETCHARFORMAT,SCF_SELECTION,(LPARAM)&keyFormat);
    check(keyFormat.crTextColor==RGB(29,78,160)&&(keyFormat.dwEffects&CFE_BOLD),"JSON keys have distinct bold syntax coloring");
    auto hello=rendered.find(L"hello");SendMessageW(gResponseBody,EM_SETSEL,(WPARAM)hello,(LPARAM)(hello+5));
    CHARFORMAT2W stringFormat{};stringFormat.cbSize=sizeof(stringFormat);SendMessageW(gResponseBody,EM_GETCHARFORMAT,SCF_SELECTION,(LPARAM)&stringFormat);
    check(stringFormat.crTextColor==RGB(21,128,61)&&!(stringFormat.dwEffects&CFE_BOLD),"JSON strings have separate coloring without bold");
    SetFocus(gResponseFind);SendMessageW(gResponseFind,BM_CLICK,0,0);SetFocus(gResponseFindEdit);setText(gResponseFindEdit,L"user_name");DWORD start=0,end=0;SendMessageW(gResponseBody,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
    check(rendered.substr(start,end-start)==L"user_name","search selects the exact word after multiple JSON line breaks");
    SendMessageW(gResponseFindClose,BM_CLICK,0,0);
    POINTL hit{};SendMessageW(gResponseBody,EM_POSFROMCHAR,(WPARAM)&hit,(LPARAM)(key+3));
    SendMessageW(gResponseBody,WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(hit.x+1,hit.y+2));SendMessageW(gResponseBody,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
    check(rendered.substr(start,end-start)==L"user_name","double click selects a complete English JSON field without quotes");
    SendMessageW(gResponseBody,EM_POSFROMCHAR,(WPARAM)&hit,(LPARAM)(hello+2));
    SendMessageW(gResponseBody,WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(hit.x+1,hit.y+2));SendMessageW(gResponseBody,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
    check(rendered.substr(start,end-start)==L"hello","double click stops at punctuation inside JSON strings");
    tab->rawView=true;showResponsePage();check(responseText()==toWide(tab->responseRaw),"raw view preserves the original JSON text");
    SendMessageW(gResponseBody,EM_SETSEL,0,-1);CHARFORMAT2W rawFormat{};rawFormat.cbSize=sizeof(rawFormat);SendMessageW(gResponseBody,EM_GETCHARFORMAT,SCF_SELECTION,(LPARAM)&rawFormat);
    check((rawFormat.dwMask&CFM_COLOR)&&rawFormat.crTextColor==COLOR_PRIMARY&&!(rawFormat.dwEffects&CFE_BOLD),"raw view clears syntax formatting");
    tab->rawView=false;showResponsePage();
    setText(gUrl,L"not-a-url");const auto oldBody=tab->responseRaw;sendCurrent();
    check(!tab->sending&&tab->responseRaw==oldBody&&!tab->validation.empty(),"invalid URL is rejected without removing the last response");
    setText(gUrl,L"http://127.0.0.1/test");tab->caseSnapshot->bodyType="JSON";tab->editorPage=2;loadEditor();setText(gBody,L"{");sendCurrent();
    check(!tab->sending&&tab->responseRaw==oldBody&&gEditorPage==2,"invalid JSON is rejected before transport and preserves response");
    setText(gBody,L"{}");finish(tab,false,true);
    check(tab->summary.find(L"已取消")==0&&tab->responseRaw==oldBody&&tab->oldResponse,"cancelled requests retain explicitly labelled previous response");
    finish(tab,false);check(tab->summary.find(L"网络错误")==0&&tab->responseRaw==oldBody,"network failures preserve previous response");
    finish(tab,true,false,500);check(tab->responseRaw=="{\"new\":true}"&&tab->statusCode==500&&!tab->oldResponse,"HTTP 500 replaces response normally");
    runLargeResponseFindTests(tab);runResponseFindTests(tab,request,c,second);
    tab->rawView=true;captureView();showResponsePage();
    openRequest(second);finish(tab,true,false,201);check(textOf(gUrl)==toWide(second->url),"background completion does not replace the selected request editor");
    openRequest(request,c);check(tab->rawView&&textOf(gResponseFindEdit)==L"match"&&!gResponseFindVisible,"response mode and search query belong to each tab");
    check(saveNow(),"case response can be saved");

    tab->sending=true;tab->cancel=false;tab->started=std::chrono::steady_clock::now()-std::chrono::milliseconds(200);
    SendMessageW(gWindow,WM_TIMER,REQUEST_TIMER,0);check(tab->summary.find(L"请求中")!=wstring::npos,"request timer updates elapsed status");
    cancelCurrent();check(tab->cancel&&tab->summary==L"正在取消","cancel button immediately enters cancelling state");finish(tab,false,true);
    check(saveNow(),"request activity does not invalidate saving response content");

    setText(gUrl,L"http://127.0.0.1/unsaved");size_t count=gTabs.size();closeChoice=IDCANCEL;closeTab(gSelectedTab);
    check(gTabs.size()==count&&tabDirty(*tab),"cancel close keeps all tabs and edits");
    closeChoice=IDYES;failSave=true;closeTab(gSelectedTab);check(gTabs.size()==count&&tabDirty(*tab),"save failure prevents closing");failSave=false;
    closeChoice=IDNO;closeTab(gSelectedTab);openRequest(request,c);check(!tabDirty(*selectedTab())&&c->url!="http://127.0.0.1/unsaved","discard restores the saved case and reopens cleanly");
    setText(gUrl,L"http://127.0.0.1/dirty-case");openRequest(second);setText(gBody,L"{\"changed\":true}");saveEditor();
    int prompts=closePrompts;count=gTabs.size();closeChoice=IDCANCEL;closeTabRange(gSelectedTab,IDM_TAB_CLOSE_ALL);
    check(closePrompts==prompts+1&&gTabs.size()==count,"batch close asks once and cancels atomically");
    closeChoice=IDNO;closeTabRange(gSelectedTab,IDM_TAB_CLOSE_ALL);check(gTabs.empty()&&!workspaceDirty(),"batch discard restores all target drafts");
    RECT saveBounds=boundsInWindow(gSave),emptyClient{};GetClientRect(gWindow,&emptyClient);
    check(IsWindowEnabled(gSave)&&(GetWindowLongPtrW(gSave,GWL_STYLE)&WS_VISIBLE)&&(GetWindowLongPtrW(gSave,GWL_STYLE)&WS_TABSTOP)&&
          saveBounds.left>=0&&saveBounds.top>=0&&saveBounds.right<=emptyClient.right&&saveBounds.bottom<=emptyClient.bottom&&
          saveBounds.right>saveBounds.left&&saveBounds.bottom>saveBounds.top,"workspace save remains visible and keyboard-accessible when all request tabs are closed");
    RECT createBounds=boundsInWindow(gEmptyNewRequest),treeBounds=boundsInWindow(gTree);
    check((GetWindowLongPtrW(gEmptyNewRequest,GWL_STYLE)&WS_VISIBLE)&&IsWindowEnabled(gEmptyNewRequest)&&textOf(gEmptyNewRequest)==L"新建接口"&&
          createBounds.left>=treeBounds.right&&createBounds.right<=emptyClient.right&&
          createBounds.top>=0&&createBounds.bottom<=emptyClient.bottom,
          "empty workspace offers a visible new-request action inside the main panel");

    auto fresh=std::make_unique<ApiRequest>();fresh->id="new";fresh->name="未保存接口";auto freshPtr=fresh.get();gData.folders[0]->requests.push_back(std::move(fresh));openRequest(freshPtr);
    check(!(GetWindowLongPtrW(gEmptyNewRequest,GWL_STYLE)&WS_VISIBLE),"new-request empty action hides while an editor is open");
    closeChoice=IDNO;closeTab(gSelectedTab);check(gData.folders[0]->requests.size()==2&&gTabs.empty(),"discarding an unsaved new request removes it");
    openRequest(request);setText(gUrl,L"http://127.0.0.1/exit");closeChoice=IDCANCEL;SendMessageW(gWindow,WM_CLOSE,0,0);
    check(IsWindow(gWindow)&&workspaceDirty(),"cancel exit preserves the session");
    closeChoice=IDYES;failSave=true;SendMessageW(gWindow,WM_CLOSE,0,0);check(IsWindow(gWindow)&&workspaceDirty(),"failed exit save keeps the window open");failSave=false;
    closeChoice=IDNO;SendMessageW(gWindow,WM_CLOSE,0,0);check(!IsWindow(gWindow),"explicit discard exits");
}
int main(int argc,char**) {
    std::cout.setf(std::ios::unitbuf);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    gServices={loadTestData,saveTestData,testTaskDialog};
    testMode=argc>1;SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);
    auto testRoot=std::filesystem::path(executable).parent_path()/L"ui-sandbox";
    if(testMode)testRoot/=toWide(newId());testDataPath=(testRoot/L"data.json").wstring();
    if(!testMode){CoUninitialize();return runMainWindow(GetModuleHandleW(nullptr),SW_SHOWNORMAL,gServices);}
    gInstance=GetModuleHandleW(nullptr);gDpi=GetDpiForSystem();
    WNDCLASSW find{};find.hInstance=gInstance;find.lpfnWndProc=responseFindPanelProc;find.lpszClassName=L"FeatherApiResponseFind";find.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&find);
    WNDCLASSW wc{};wc.hInstance=gInstance;wc.lpfnWndProc=windowProc;wc.lpszClassName=L"FeatherApiControllerTests";RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"FeatherApi tests",WS_OVERLAPPEDWINDOW,0,0,1280,820,nullptr,nullptr,gInstance,nullptr);
    check(window!=nullptr,"test window creates with isolated data");if(window)runControllerTests();
    if(gRichEditModule){FreeLibrary(gRichEditModule);gRichEditModule=nullptr;}
    CoUninitialize();std::cout<<failures<<" failure(s)\n";return failures?1:0;
}
