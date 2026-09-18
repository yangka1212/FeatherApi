#include "infrastructure/storage.h"
#include "domain/normalization.h"
#include "json/detail/value.h"
#include "platform/identity.h"
#include "platform/windows.h"
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <ctime>

using namespace feather::json_detail;

static wstring localAppData() {
    PWSTR path = nullptr;
    wstring result;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path) == S_OK) {
        result = path;
        CoTaskMemFree(path);
    }
    return result;
}

wstring win32DataPath() { return localAppData() + L"\\FeatherApi-Win32\\data.json"; }

namespace {
string getStoredText(const Json& object,const char* name,const string& fallback={}){auto p=object.getInsensitive(name);return p?p->text(fallback):fallback;}
bool getStoredBool(const Json& object,const char* name,bool fallback=true){auto p=object.getInsensitive(name);return p?p->boolean(fallback):fallback;}
string normalizedId(string value){return trimText(value).empty()?newId():value;}
std::vector<KeyValueEntry> readEntries(const Json* json) {
    std::vector<KeyValueEntry> result; if(!json||!json->array())return result;
    for(const auto& item:*json->array()) result.push_back({getStoredBool(item,"IsEnabled",getStoredBool(item,"Enabled",true)),getStoredText(item,"Key"),getStoredText(item,"Value"),normalizedParameterType(getStoredText(item,"Type")),getStoredText(item,"Description")});
    return result;
}

ApiRequestCase readCase(const Json& json) {
    ApiRequestCase c; c.id=normalizedId(getStoredText(json,"Id")); c.name=normalizedName(getStoredText(json,"Name"),"请求用例"); c.method=normalizedMethod(getStoredText(json,"Method")); c.url=getStoredText(json,"Url");
    c.query=readEntries(json.getInsensitive("QueryParams")); c.headers=readEntries(json.getInsensitive("Headers")); c.bodyType=getStoredText(json,"BodyType","None");if(trimText(c.bodyType).empty())c.bodyType="None";c.body=getStoredText(json,"BodyContent"); c.formFields=readEntries(json.getInsensitive("FormFields"));
    c.responseSummary=getStoredText(json,"ResponseSummary","暂无响应"); c.responseRaw=getStoredText(json,"ResponseRawBody"); c.responsePretty=getStoredText(json,"ResponsePrettyBody"); c.responseHeaders=readEntries(json.getInsensitive("ResponseHeaders")); return c;
}

std::unique_ptr<ApiRequest> readRequest(const Json& json) {
    auto r=std::make_unique<ApiRequest>();r->id=normalizedId(getStoredText(json,"Id"));r->name=normalizedName(getStoredText(json,"Name"),"未命名接口");r->method=normalizedMethod(getStoredText(json,"Method"));r->url=getStoredText(json,"Url");
    r->query=readEntries(json.getInsensitive("QueryParams"));r->headers=readEntries(json.getInsensitive("Headers"));r->bodyType=getStoredText(json,"BodyType","None");if(trimText(r->bodyType).empty())r->bodyType="None";r->body=getStoredText(json,"BodyContent");r->formFields=readEntries(json.getInsensitive("FormFields"));
    if(auto cases=json.getInsensitive("Cases");cases&&cases->array())for(const auto& c:*cases->array())r->cases.push_back(std::make_unique<ApiRequestCase>(readCase(c)));return r;
}

std::unique_ptr<ApiFolder> readFolder(const Json& json) {
    auto f=std::make_unique<ApiFolder>();f->id=normalizedId(getStoredText(json,"Id"));f->name=normalizedName(getStoredText(json,"Name"),"未命名目录");f->expanded=getStoredBool(json,"IsExpanded",false);
    if(auto children=json.getInsensitive("Children");children&&children->array())for(const auto& child:*children->array())f->children.push_back(readFolder(child));
    if(auto requests=json.getInsensitive("Requests");requests&&requests->array())for(const auto& request:*requests->array())f->requests.push_back(readRequest(request));return f;
}

void writeEntries(std::ostream& out,const std::vector<KeyValueEntry>& values){out<<'[';for(size_t i=0;i<values.size();++i){if(i)out<<',';out<<"{\"IsEnabled\":"<<(values[i].enabled?"true":"false")<<",\"Key\":";escape(out,values[i].key);out<<",\"Value\":";escape(out,values[i].value);out<<",\"Type\":";escape(out,values[i].type);out<<",\"Description\":";escape(out,values[i].description);out<<'}';}out<<']';}
void writeCase(std::ostream& out,const ApiRequestCase& c){out<<"{\"Id\":";escape(out,c.id);out<<",\"Name\":";escape(out,c.name);out<<",\"Method\":";escape(out,c.method);out<<",\"Url\":";escape(out,c.url);out<<",\"QueryParams\":";writeEntries(out,c.query);out<<",\"Headers\":";writeEntries(out,c.headers);out<<",\"BodyType\":";escape(out,c.bodyType);out<<",\"BodyContent\":";escape(out,c.body);out<<",\"FormFields\":";writeEntries(out,c.formFields);out<<",\"ResponseSummary\":";escape(out,c.responseSummary);out<<",\"ResponseRawBody\":";escape(out,c.responseRaw);out<<",\"ResponsePrettyBody\":";escape(out,c.responsePretty);out<<",\"ResponseHeaders\":";writeEntries(out,c.responseHeaders);out<<'}';}
void writeRequest(std::ostream& out,const ApiRequest& r){out<<"{\"Id\":";escape(out,r.id);out<<",\"Name\":";escape(out,r.name);out<<",\"Method\":";escape(out,r.method);out<<",\"Url\":";escape(out,r.url);out<<",\"QueryParams\":";writeEntries(out,r.query);out<<",\"Headers\":";writeEntries(out,r.headers);out<<",\"BodyType\":";escape(out,r.bodyType);out<<",\"BodyContent\":";escape(out,r.body);out<<",\"FormFields\":";writeEntries(out,r.formFields);out<<",\"Cases\":[";for(size_t i=0;i<r.cases.size();++i){if(i)out<<',';writeCase(out,*r.cases[i]);}out<<"]}";}
void writeFolder(std::ostream& out,const ApiFolder& f){out<<"{\"Id\":";escape(out,f.id);out<<",\"Name\":";escape(out,f.name);out<<",\"IsExpanded\":"<<(f.expanded?"true":"false")<<",\"Children\":[";for(size_t i=0;i<f.children.size();++i){if(i)out<<',';writeFolder(out,*f.children[i]);}out<<"],\"Requests\":[";for(size_t i=0;i<f.requests.size();++i){if(i)out<<',';writeRequest(out,*f.requests[i]);}out<<"]}";}

}

bool loadAppDataFromPath(const wstring& path,AppData& data) {
    std::ifstream input(std::filesystem::path(path),std::ios::binary);if(!input)return false;
    string source((std::istreambuf_iterator<char>(input)),{});if(source.size()>=3&&(unsigned char)source[0]==0xEF&&(unsigned char)source[1]==0xBB&&(unsigned char)source[2]==0xBF)source.erase(0,3);
    Json root;if(!Parser(source).parse(root)||!root.object())return false;AppData loaded;
    if(auto settings=root.getInsensitive("Settings")){if(auto p=settings->getInsensitive("SidebarWidth"))loaded.sidebarWidth=std::clamp(p->integer(320),180,420);if(auto p=settings->getInsensitive("RequestPanelHeight"))loaded.requestPanelHeight=std::max(180,p->integer(330));}
    if(auto folders=root.getInsensitive("Folders");folders&&folders->array())for(const auto& folder:*folders->array())loaded.folders.push_back(readFolder(folder));
    if(loaded.folders.empty()){auto folder=std::make_unique<ApiFolder>();folder->id=newId();folder->name="默认目录";loaded.folders.push_back(std::move(folder));}data=std::move(loaded);return true;
}

bool saveAppDataToPath(const AppData& data,const wstring& filePath) {
    auto path=std::filesystem::path(filePath);std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);
    auto temporary=path;temporary+=L".tmp";std::ofstream out(temporary,std::ios::binary|std::ios::trunc);if(!out)return false;
    out<<"{\"Settings\":{\"SidebarWidth\":"<<data.sidebarWidth<<",\"RequestPanelHeight\":"<<data.requestPanelHeight<<"},\"Folders\":[";
    for(size_t i=0;i<data.folders.size();++i){if(i)out<<',';writeFolder(out,*data.folders[i]);}out<<"]}";out.close();if(!out)return false;
    return MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
}

bool loadAppData(AppData& data,wstring& sourcePath) {
    auto native=std::filesystem::path(win32DataPath());
    if(std::filesystem::exists(native)){
        if(loadAppDataFromPath(native.wstring(),data)){sourcePath=native.wstring();return true;}
        auto now=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());std::tm local{};localtime_s(&local,&now);wchar_t suffix[64]{};wcsftime(suffix,64,L".corrupt-%Y%m%d-%H%M%S",&local);std::error_code ec;std::filesystem::copy_file(native,native.wstring()+suffix,std::filesystem::copy_options::overwrite_existing,ec);sourcePath.clear();return false;
    }
    sourcePath.clear();return false;
}

bool saveAppData(const AppData& data){return saveAppDataToPath(data,win32DataPath());}
