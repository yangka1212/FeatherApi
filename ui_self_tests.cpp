// Exercise the actual Win32 controllers against an isolated fixture. No user data is loaded or saved.
#include "app.h"
#include <commctrl.h>
#include <filesystem>
#include <iostream>

static bool testMode=false,failSave=false;
static int closeChoice=IDCANCEL,closePrompts=0,saveCalls=0,failures=0;
static std::wstring testDataPath;
bool loadTestData(AppData&,std::wstring&);
bool saveTestData(const AppData&);
HRESULT WINAPI testTaskDialog(const TASKDIALOGCONFIG*,int*,int*,BOOL*);

// Keep persistence and modal decisions injectable without exposing testing switches in the application.
#define loadAppData loadTestData
#define saveAppData saveTestData
#define TaskDialogIndirect testTaskDialog
#include "main_window.cpp"
#undef loadAppData
#undef saveAppData
#undef TaskDialogIndirect

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
static void runControllerTests() {
    auto request=gData.folders[0]->requests[0].get();auto second=gData.folders[0]->requests[1].get();auto c=request->cases[0].get();
    check(!workspaceDirty(),"fixture starts clean");
    setText(gUrl,L"http://127.0.0.1/edited");updateDirtyStatus();check(tabDirty(*selectedTab())&&gSaveStatus==L"未保存","URL edits immediately mark the tab and workspace dirty");
    TreeView_Expand(gTree,TreeView_GetRoot(gTree),TVE_COLLAPSE);check(saveCalls==0,"expanding or collapsing a folder never saves pending request edits");
    TreeView_Expand(gTree,TreeView_GetRoot(gTree),TVE_EXPAND);
    setText(gUrl,toWide(gSavedContent.requests.at("request").url));updateDirtyStatus();check(!workspaceDirty(),"restoring original URL clears dirty state");

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
    AppData disk;check(loadAppDataFromPath(testDataPath,disk)&&disk.folders[0]->requests[0]->cases[0]->url=="http://127.0.0.1/case-only","failed save leaves the file unchanged");
    failSave=false;check(saveNow()&&!workspaceDirty()&&!gSaveFailed,"retry saves the retained case draft");

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
    setText(gResponseFindEdit,L"user_name");findInResponseBody(true);DWORD start=0,end=0;SendMessageW(gResponseBody,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
    check(rendered.substr(start,end-start)==L"user_name","search selects the exact word after multiple JSON line breaks");
    POINTL hit{};SendMessageW(gResponseBody,EM_POSFROMCHAR,(WPARAM)&hit,(LPARAM)(key+3));
    SendMessageW(gResponseBody,WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(hit.x+1,hit.y+2));SendMessageW(gResponseBody,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
    check(rendered.substr(start,end-start)==L"user_name","double click selects a complete English JSON field without quotes");
    SendMessageW(gResponseBody,EM_POSFROMCHAR,(WPARAM)&hit,(LPARAM)(hello+2));
    SendMessageW(gResponseBody,WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(hit.x+1,hit.y+2));SendMessageW(gResponseBody,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
    check(rendered.substr(start,end-start)==L"hello","double click stops at punctuation inside JSON strings");
    tab->rawView=true;showResponsePage();check(responseText()==toWide(tab->responseRaw),"raw view preserves the original JSON text");
    SendMessageW(gResponseBody,EM_SETSEL,0,-1);CHARFORMAT2W rawFormat{};rawFormat.cbSize=sizeof(rawFormat);SendMessageW(gResponseBody,EM_GETCHARFORMAT,SCF_SELECTION,(LPARAM)&rawFormat);
    check((rawFormat.dwMask&CFM_COLOR)&&rawFormat.crTextColor==RGB(51,65,85)&&!(rawFormat.dwEffects&CFE_BOLD),"raw view clears syntax formatting");
    tab->rawView=false;showResponsePage();
    setText(gUrl,L"not-a-url");const auto oldBody=tab->responseRaw;sendCurrent();
    check(!tab->sending&&tab->responseRaw==oldBody&&!tab->validation.empty(),"invalid URL is rejected without removing the last response");
    setText(gUrl,L"http://127.0.0.1/test");tab->caseSnapshot->bodyType="JSON";tab->editorPage=2;loadEditor();setText(gBody,L"{");sendCurrent();
    check(!tab->sending&&tab->responseRaw==oldBody&&gEditorPage==2,"invalid JSON is rejected before transport and preserves response");
    setText(gBody,L"{}");finish(tab,false,true);
    check(tab->summary.find(L"已取消")==0&&tab->responseRaw==oldBody&&tab->oldResponse,"cancelled requests retain explicitly labelled previous response");
    finish(tab,false);check(tab->summary.find(L"网络错误")==0&&tab->responseRaw==oldBody,"network failures preserve previous response");
    finish(tab,true,false,500);check(tab->responseRaw=="{\"new\":true}"&&tab->statusCode==500&&!tab->oldResponse,"HTTP 500 replaces response normally");
    tab->findVisible=true;gResponseFindVisible=true;setText(gResponseFindEdit,L"new");findInResponseBody(true);
    check(textOf(gFindStatus)==L"1 / 1","response search shows current match and total");findInResponseBody(true);check(textOf(gFindStatus).find(L"已循环")!=wstring::npos,"search wrap is visible");
    setText(gResponseFindEdit,L"missing");findInResponseBody(true);check(textOf(gFindStatus)==L"未找到","missing search term has visible feedback");
    setText(gResponseFindEdit,L"new");findInResponseBody(true);tab->rawView=true;captureView();showResponsePage();
    openRequest(second);finish(tab,true,false,201);check(textOf(gUrl)==toWide(second->url),"background completion does not replace the selected request editor");
    openRequest(request,c);check(tab->rawView&&textOf(gResponseFindEdit)==L"new"&&gResponseFindVisible,"response mode and search query belong to each tab");
    check(saveNow(),"case response can be saved");

    for(UINT dpi:{96u,144u,192u}){
        RECT proposed{0,0,MulDiv(980,(int)dpi,96),MulDiv(640,(int)dpi,96)};
        SendMessageW(gWindow,WM_DPICHANGED,MAKEWPARAM(dpi,dpi),(LPARAM)&proposed);
        RECT body{},find{},client{};GetWindowRect(gResponseBody,&body);GetWindowRect(gResponseFindPanel,&find);GetClientRect(gWindow,&client);
        MapWindowPoints(nullptr,gWindow,(POINT*)&body,2);MapWindowPoints(nullptr,gWindow,(POINT*)&find,2);
        check(body.top>=find.bottom&&body.bottom<=client.bottom&&body.right<=client.right&&body.bottom>body.top,"minimum-window layout keeps search above response at 100/150/200 percent");
    }
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

    auto fresh=std::make_unique<ApiRequest>();fresh->id="new";fresh->name="未保存接口";auto freshPtr=fresh.get();gData.folders[0]->requests.push_back(std::move(fresh));openRequest(freshPtr);
    closeChoice=IDNO;closeTab(gSelectedTab);check(gData.folders[0]->requests.size()==2&&gTabs.empty(),"discarding an unsaved new request removes it");
    openRequest(request);setText(gUrl,L"http://127.0.0.1/exit");closeChoice=IDCANCEL;SendMessageW(gWindow,WM_CLOSE,0,0);
    check(IsWindow(gWindow)&&workspaceDirty(),"cancel exit preserves the session");
    closeChoice=IDYES;failSave=true;SendMessageW(gWindow,WM_CLOSE,0,0);check(IsWindow(gWindow)&&workspaceDirty(),"failed exit save keeps the window open");failSave=false;
    closeChoice=IDNO;SendMessageW(gWindow,WM_CLOSE,0,0);check(!IsWindow(gWindow),"explicit discard exits");
}
int main(int argc,char**) {
    std::cout.setf(std::ios::unitbuf);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    testMode=argc>1;SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);
    auto testRoot=std::filesystem::path(executable).parent_path()/L"ui-sandbox";
    if(testMode)testRoot/=toWide(newId());testDataPath=(testRoot/L"data.json").wstring();
    if(!testMode){CoUninitialize();return runMainWindow(GetModuleHandleW(nullptr),SW_SHOWNORMAL);}
    gInstance=GetModuleHandleW(nullptr);gDpi=GetDpiForSystem();
    WNDCLASSW find{};find.hInstance=gInstance;find.lpfnWndProc=responseFindPanelProc;find.lpszClassName=L"FeatherApiResponseFind";find.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&find);
    WNDCLASSW wc{};wc.hInstance=gInstance;wc.lpfnWndProc=windowProc;wc.lpszClassName=L"FeatherApiControllerTests";RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"FeatherApi tests",WS_OVERLAPPEDWINDOW,0,0,1280,820,nullptr,nullptr,gInstance,nullptr);
    check(window!=nullptr,"test window creates with isolated data");if(window)runControllerTests();
    if(gRichEditModule){FreeLibrary(gRichEditModule);gRichEditModule=nullptr;}
    CoUninitialize();std::cout<<failures<<" failure(s)\n";return failures?1:0;
}
