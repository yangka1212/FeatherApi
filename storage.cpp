#include "app.h"

#include <shlobj.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <cctype>
#include <chrono>
#include <ctime>
#include <sstream>
#include <variant>
#include <limits>
#include <locale>

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

string newId() {
    GUID guid{};
    CoCreateGuid(&guid);
    char text[33]{};
    snprintf(text, sizeof(text), "%08x%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
        guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return text;
}

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
struct JsonNumber { string token; };

struct Json {
    using Object = std::vector<std::pair<string, Json>>;
    using Array = std::vector<Json>;
    std::variant<std::nullptr_t, bool, JsonNumber, string, Object, Array> value;
    Json() : value(nullptr) {}
    Json(bool v) : value(v) {}
    Json(JsonNumber v) : value(std::move(v)) {}
    Json(string v) : value(std::move(v)) {}
    Json(Object v) : value(std::move(v)) {}
    Json(Array v) : value(std::move(v)) {}
    const Object* object() const { return std::get_if<Object>(&value); }
    const Array* array() const { return std::get_if<Array>(&value); }
    string text(const string& fallback = {}) const { auto p=std::get_if<string>(&value); return p?*p:fallback; }
    bool boolean(bool fallback=true) const { auto p=std::get_if<bool>(&value); return p?*p:fallback; }
    int integer(int fallback) const {
        auto p=std::get_if<JsonNumber>(&value);if(!p)return fallback;
        std::istringstream input(p->token);input.imbue(std::locale::classic());double parsed=0;
        if(!(input>>parsed)||!std::isfinite(parsed)||parsed<std::numeric_limits<int>::min()||parsed>std::numeric_limits<int>::max())return fallback;
        return (int)parsed;
    }
    const Json* get(const char* key) const { auto o=object();if(!o)return nullptr;for(const auto& entry:*o)if(entry.first==key)return &entry.second;return nullptr; }
    const Json* getInsensitive(const char* key) const { auto o=object();if(!o)return nullptr;for(const auto& entry:*o)if(_stricmp(entry.first.c_str(),key)==0)return &entry.second;return nullptr; }
};

class Parser {
public:
    explicit Parser(const string& source) : source_(source) {}
    bool parse(Json& output) { skip(); if(!value(output))return false; skip(); return position_==source_.size(); }
    size_t errorPosition() const { return position_; }
private:
    const string& source_; size_t position_=0;
    void skip(){while(position_<source_.size()&&(source_[position_]==' '||source_[position_]=='\t'||source_[position_]=='\r'||source_[position_]=='\n'))++position_;}
    bool take(char c){skip();if(position_<source_.size()&&source_[position_]==c){++position_;return true;}return false;}
    bool value(Json& out){
        skip(); if(position_>=source_.size())return false; char c=source_[position_];
        if(c=='{')return object(out); if(c=='[')return array(out); if(c=='"'){string s;if(!quoted(s))return false;out=Json(std::move(s));return true;}
        if(c=='t'&&source_.compare(position_,4,"true")==0){position_+=4;out=Json(true);return true;}
        if(c=='f'&&source_.compare(position_,5,"false")==0){position_+=5;out=Json(false);return true;}
        if(c=='n'&&source_.compare(position_,4,"null")==0){position_+=4;out=Json();return true;}
        return number(out);
    }
    bool object(Json& out){
        if(!take('{'))return false; Json::Object result; skip(); if(take('}')){out=Json(std::move(result));return true;}
        for(;;){string key;if(!quoted(key)||!take(':'))return false;Json item;if(!value(item))return false;auto existing=std::find_if(result.begin(),result.end(),[&](const auto& entry){return entry.first==key;});if(existing==result.end())result.emplace_back(std::move(key),std::move(item));else existing->second=std::move(item);if(take('}'))break;if(!take(','))return false;}
        out=Json(std::move(result));return true;
    }
    bool array(Json& out){
        if(!take('['))return false; Json::Array result; skip();if(take(']')){out=Json(std::move(result));return true;}
        for(;;){Json item;if(!value(item))return false;result.push_back(std::move(item));if(take(']'))break;if(!take(','))return false;}
        out=Json(std::move(result));return true;
    }
    bool quoted(string& out){
        skip();if(position_>=source_.size()||source_[position_++]!='"')return false;out.clear();
        while(position_<source_.size()){
            unsigned char c=(unsigned char)source_[position_++];if(c=='"')return true;if(c<0x20)return false;if(c!='\\'){out+=(char)c;continue;}
            if(position_>=source_.size())return false;char e=source_[position_++];
            switch(e){case '"':out+='"';break;case '\\':out+='\\';break;case '/':out+='/';break;case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
            case 'u':{
                unsigned code=0;if(!hexCodeUnit(code))return false;
                if(code>=0xD800&&code<=0xDBFF){
                    if(position_+2>source_.size()||source_[position_]!='\\'||source_[position_+1]!='u')return false;position_+=2;
                    unsigned low=0;if(!hexCodeUnit(low)||low<0xDC00||low>0xDFFF)return false;wstring pair;pair+=(wchar_t)code;pair+=(wchar_t)low;out+=toUtf8(pair);
                }else if(code>=0xDC00&&code<=0xDFFF)return false;
                else out+=toUtf8(wstring(1,(wchar_t)code));break;
            }default:return false;}
        }return false;
    }
    bool hexCodeUnit(unsigned& code){
        if(position_+4>source_.size())return false;code=0;for(int i=0;i<4;++i){char h=source_[position_++];unsigned digit=0;if(h>='0'&&h<='9')digit=(unsigned)(h-'0');else if(h>='a'&&h<='f')digit=(unsigned)(h-'a'+10);else if(h>='A'&&h<='F')digit=(unsigned)(h-'A'+10);else return false;code=(code<<4)+digit;}return true;
    }
    bool number(Json& out){
        size_t start=position_;if(position_<source_.size()&&source_[position_]=='-')++position_;
        if(position_>=source_.size())return false;
        if(source_[position_]=='0')++position_;else if(source_[position_]>='1'&&source_[position_]<='9'){while(position_<source_.size()&&std::isdigit((unsigned char)source_[position_]))++position_;}else return false;
        if(position_<source_.size()&&source_[position_]=='.'){++position_;size_t fraction=position_;while(position_<source_.size()&&std::isdigit((unsigned char)source_[position_]))++position_;if(position_==fraction)return false;}
        if(position_<source_.size()&&(source_[position_]=='e'||source_[position_]=='E')){++position_;if(position_<source_.size()&&(source_[position_]=='+'||source_[position_]=='-'))++position_;size_t exponent=position_;while(position_<source_.size()&&std::isdigit((unsigned char)source_[position_]))++position_;if(position_==exponent)return false;}
        out=Json(JsonNumber{source_.substr(start,position_-start)});return true;
    }
};

string getStoredText(const Json& object,const char* name,const string& fallback={}){auto p=object.getInsensitive(name);return p?p->text(fallback):fallback;}
bool getStoredBool(const Json& object,const char* name,bool fallback=true){auto p=object.getInsensitive(name);return p?p->boolean(fallback):fallback;}
string getText(const Json& object, const char* name, const string& fallback={}) { auto p=object.get(name);return p?p->text(fallback):fallback; }
string trimText(string value) { while(!value.empty()&&std::isspace((unsigned char)value.front()))value.erase(value.begin());while(!value.empty()&&std::isspace((unsigned char)value.back()))value.pop_back();return value; }
bool equalsIgnoreCase(const string& left,const string& right){return _stricmp(left.c_str(),right.c_str())==0;}
bool containsIgnoreCase(const string& value,const string& search){if(search.empty())return true;auto lower=[](unsigned char c){return (char)std::tolower(c);};return std::search(value.begin(),value.end(),search.begin(),search.end(),[&](char left,char right){return lower((unsigned char)left)==lower((unsigned char)right);})!=value.end();}
string combineUrl(string baseUrl,string path){if(baseUrl.empty())return path;while(!baseUrl.empty()&&baseUrl.back()=='/')baseUrl.pop_back();size_t start=0;while(start<path.size()&&path[start]=='/')++start;return baseUrl+"/"+path.substr(start);}
string sourceOrigin(const string& url){size_t scheme=url.find("://");if(scheme==string::npos)return {};size_t authority=url.find('/',scheme+3);return url.substr(0,authority==string::npos?url.size():authority);}
string resolveImportBase(string baseUrl,const string& sourceUrl){
    baseUrl=trimText(baseUrl);string origin=sourceOrigin(sourceUrl);
    if(baseUrl.empty())return origin;
    if(baseUrl.rfind("//",0)==0){size_t scheme=sourceUrl.find("://");return scheme==string::npos?baseUrl:sourceUrl.substr(0,scheme)+":"+baseUrl;}
    if(baseUrl.find("://")!=string::npos)return baseUrl;
    return combineUrl(origin,baseUrl);
}
string normalizedId(string value){return trimText(value).empty()?newId():value;}
string normalizedName(string value,const string& fallback){value=trimText(value);return value.empty()?fallback:value;}
string normalizedMethod(string value){value=trimText(value);if(value.empty())return "GET";for(char& c:value)c=(char)std::toupper((unsigned char)c);return value;}
string normalizedParameterType(string value){value=trimText(value);if(equalsIgnoreCase(value,"file")||containsIgnoreCase(value,"binary"))return "file";if(equalsIgnoreCase(value,"number")||equalsIgnoreCase(value,"int")||equalsIgnoreCase(value,"integer")||containsIgnoreCase(value,"int32")||containsIgnoreCase(value,"int64")||equalsIgnoreCase(value,"float")||equalsIgnoreCase(value,"double")||equalsIgnoreCase(value,"decimal"))return "number";return "string";}

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

void escape(std::ostream& out,const string& value){out<<'"';for(unsigned char c:value){switch(c){case '"':out<<"\\\"";break;case '\\':out<<"\\\\";break;case '\b':out<<"\\b";break;case '\f':out<<"\\f";break;case '\n':out<<"\\n";break;case '\r':out<<"\\r";break;case '\t':out<<"\\t";break;default:if(c<0x20)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<(int)c<<std::dec;else out<<(char)c;}}out<<'"';}
void writeEntries(std::ostream& out,const std::vector<KeyValueEntry>& values){out<<'[';for(size_t i=0;i<values.size();++i){if(i)out<<',';out<<"{\"IsEnabled\":"<<(values[i].enabled?"true":"false")<<",\"Key\":";escape(out,values[i].key);out<<",\"Value\":";escape(out,values[i].value);out<<",\"Type\":";escape(out,values[i].type);out<<",\"Description\":";escape(out,values[i].description);out<<'}';}out<<']';}
void writeCase(std::ostream& out,const ApiRequestCase& c){out<<"{\"Id\":";escape(out,c.id);out<<",\"Name\":";escape(out,c.name);out<<",\"Method\":";escape(out,c.method);out<<",\"Url\":";escape(out,c.url);out<<",\"QueryParams\":";writeEntries(out,c.query);out<<",\"Headers\":";writeEntries(out,c.headers);out<<",\"BodyType\":";escape(out,c.bodyType);out<<",\"BodyContent\":";escape(out,c.body);out<<",\"FormFields\":";writeEntries(out,c.formFields);out<<",\"ResponseSummary\":";escape(out,c.responseSummary);out<<",\"ResponseRawBody\":";escape(out,c.responseRaw);out<<",\"ResponsePrettyBody\":";escape(out,c.responsePretty);out<<",\"ResponseHeaders\":";writeEntries(out,c.responseHeaders);out<<'}';}
void writeRequest(std::ostream& out,const ApiRequest& r){out<<"{\"Id\":";escape(out,r.id);out<<",\"Name\":";escape(out,r.name);out<<",\"Method\":";escape(out,r.method);out<<",\"Url\":";escape(out,r.url);out<<",\"QueryParams\":";writeEntries(out,r.query);out<<",\"Headers\":";writeEntries(out,r.headers);out<<",\"BodyType\":";escape(out,r.bodyType);out<<",\"BodyContent\":";escape(out,r.body);out<<",\"FormFields\":";writeEntries(out,r.formFields);out<<",\"Cases\":[";for(size_t i=0;i<r.cases.size();++i){if(i)out<<',';writeCase(out,*r.cases[i]);}out<<"]}";}
void writeFolder(std::ostream& out,const ApiFolder& f){out<<"{\"Id\":";escape(out,f.id);out<<",\"Name\":";escape(out,f.name);out<<",\"IsExpanded\":"<<(f.expanded?"true":"false")<<",\"Children\":[";for(size_t i=0;i<f.children.size();++i){if(i)out<<',';writeFolder(out,*f.children[i]);}out<<"],\"Requests\":[";for(size_t i=0;i<f.requests.size();++i){if(i)out<<',';writeRequest(out,*f.requests[i]);}out<<"]}";}

const Json* resolveRef(const Json& root,const Json* value,int depth=0) {
    if(!value||depth>16||!value->object())return value;
    auto reference=value->get("$ref");if(!reference)return value;string path=reference->text();
    if(path.rfind("#/",0)!=0)return value;const Json* current=&root;size_t start=2;
    while(start<=path.size()){
        size_t slash=path.find('/',start);string segment=path.substr(start,slash==string::npos?string::npos:slash-start);
        size_t pos=0;while((pos=segment.find("~1",pos))!=string::npos)segment.replace(pos,2,"/");pos=0;while((pos=segment.find("~0",pos))!=string::npos)segment.replace(pos,2,"~");
        current=current?current->get(segment.c_str()):nullptr;if(!current)return value;if(slash==string::npos)break;start=slash+1;
    }
    return resolveRef(root,current,depth+1);
}

void writeJson(std::ostream& out,const Json& value,int indent=0) {
    if(std::holds_alternative<std::nullptr_t>(value.value)){out<<"null";return;}
    if(auto boolean=std::get_if<bool>(&value.value)){out<<(*boolean?"true":"false");return;}
    if(auto number=std::get_if<JsonNumber>(&value.value)){out<<number->token;return;}
    if(auto text=std::get_if<string>(&value.value)){escape(out,*text);return;}
    if(auto array=value.array()){out<<'[';for(size_t i=0;i<array->size();++i){if(i)out<<',';out<<'\n'<<string((size_t)indent+2,' ');writeJson(out,(*array)[i],indent+2);}if(!array->empty())out<<'\n'<<string((size_t)indent,' ');out<<']';return;}
    auto object=value.object();out<<'{';size_t index=0;for(const auto& entry:*object){if(index++)out<<',';out<<'\n'<<string((size_t)indent+2,' ');escape(out,entry.first);out<<": ";writeJson(out,entry.second,indent+2);}if(object&&!object->empty())out<<'\n'<<string((size_t)indent,' ');out<<'}';
}
string jsonText(const Json& value){std::ostringstream out;writeJson(out,value);return out.str();}

Json sampleFromSchema(const Json& root,const Json* source,int depth=0) {
    if(depth>4||!source)return Json();const Json* schema=resolveRef(root,source);
    if(auto example=schema->get("example"))return *resolveRef(root,example);
    string type=getText(*schema,"type");
    if(type=="array"){Json::Array values;if(auto items=schema->get("items"))values.push_back(sampleFromSchema(root,items,depth+1));return Json(std::move(values));}
    if(type=="object"||schema->get("properties")){Json::Object values;if(auto properties=schema->get("properties");properties&&properties->object())for(const auto& property:*properties->object())values.emplace_back(property.first,sampleFromSchema(root,&property.second,depth+1));return Json(std::move(values));}
    if(type=="integer"||type=="number")return Json(JsonNumber{"0"});if(type=="boolean")return Json(false);return Json(string());
}

string scalarText(const Json* value) {
    if(!value)return {};if(auto text=std::get_if<string>(&value->value))return *text;if(auto boolean=std::get_if<bool>(&value->value))return *boolean?"true":"false";
    if(auto number=std::get_if<JsonNumber>(&value->value))return number->token;return jsonText(*value);
}

string parameterValue(const Json& root,const Json& parameter) {
    if(auto example=parameter.get("example"))return scalarText(example);
    if(auto value=parameter.get("default"))return scalarText(value);
    if(auto schema=resolveRef(root,parameter.get("schema")))if(auto value=schema->get("default"))return scalarText(value);
    return {};
}

string parameterType(const Json& root,const Json& parameter) {
    const Json* schema=resolveRef(root,parameter.get("schema"));string type=schema?getText(*schema,"type"):getText(parameter,"type");
    string format=schema?getText(*schema,"format"):getText(parameter,"format");
    if(equalsIgnoreCase(type,"integer")||equalsIgnoreCase(type,"int")||equalsIgnoreCase(type,"number"))return "number";
    if(equalsIgnoreCase(type,"file")||(equalsIgnoreCase(type,"string")&&equalsIgnoreCase(format,"binary")))return "file";
    return "string";
}
string parameterDescription(const Json& root,const Json& parameter) {
    string description=getText(parameter,"description");if(!description.empty())return description;
    if(const Json* schema=resolveRef(root,parameter.get("schema")))return getText(*schema,"description");return {};
}
void appendSchemaFields(const Json& root,const Json* unresolved,std::vector<KeyValueEntry>& fields) {
    const Json* schema=resolveRef(root,unresolved);if(!schema)return;const auto* properties=schema->get("properties");if(!properties||!properties->object())return;
    for(const auto& property:*properties->object()){const Json* resolved=resolveRef(root,&property.second);if(!resolved)continue;string type=getText(*resolved,"type"),format=getText(*resolved,"format");string mapped=(equalsIgnoreCase(type,"file")||(equalsIgnoreCase(type,"string")&&equalsIgnoreCase(format,"binary")))?"file":(equalsIgnoreCase(type,"integer")||equalsIgnoreCase(type,"number"))?"number":"string";string value;if(auto example=resolved->get("example"))value=scalarText(example);else if(auto defaultValue=resolved->get("default"))value=scalarText(defaultValue);fields.push_back({true,property.first,value,mapped,getText(*resolved,"description")});}
}

bool bodySample(const Json& root,const Json* media,string& output) {
    media=resolveRef(root,media);if(!media)return false;
    if(auto example=media->get("example")){output=jsonText(*resolveRef(root,example));return true;}
    if(auto examples=media->get("examples");examples&&examples->object()&&!examples->object()->empty()){
        const Json& first=examples->object()->begin()->second;if(auto value=first.get("value")){output=jsonText(*value);return true;}
    }
    if(auto schema=media->get("schema")){output=jsonText(sampleFromSchema(root,schema));return true;}return false;
}
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

std::unique_ptr<ApiRequest> cloneRequest(const ApiRequest& source) {
    auto result=std::make_unique<ApiRequest>();
    result->id=newId();result->name=source.name+" - 副本";result->method=source.method;result->url=source.url;
    result->query=source.query;result->headers=source.headers;result->bodyType=source.bodyType;result->body=source.body;result->formFields=source.formFields;
    for(const auto& item:source.cases){auto copy=std::make_unique<ApiRequestCase>(*item);copy->id=newId();result->cases.push_back(std::move(copy));}
    return result;
}

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
    wchar_t text[64]{};swprintf_s(text,L"%.2f",value);std::wstring number=text;
    while(!number.empty()&&number.back()==L'0')number.pop_back();if(!number.empty()&&number.back()==L'.')number.pop_back();
    return number+(bytes<1024*1024?L" KB":L" MB");
}

bool importOpenApiJson(ApiFolder& target,const string& document,bool overwrite,OpenApiImportSummary& summary,wstring& error,const string& sourceUrl) {
    Json root;if(!Parser(document).parse(root)||!root.object()){error=L"OpenAPI 文档不是有效的 JSON。";return false;}
    bool openApi=root.get("openapi")!=nullptr;string swaggerVersion=getText(root,"swagger");bool swagger=swaggerVersion.rfind("2.",0)==0;
    if(!openApi&&!swagger){error=L"文档中未找到 openapi 或 swagger 版本字段。";return false;}
    auto paths=root.get("paths");if(!paths||!paths->object()){error=L"文档中未找到 paths 节点。";return false;}
    string baseUrl;
    if(openApi){if(auto servers=root.get("servers");servers&&servers->array()&&!servers->array()->empty())baseUrl=getText(servers->array()->front(),"url");}
    else {
        string scheme="https";if(auto schemes=root.get("schemes");schemes&&schemes->array()&&!schemes->array()->empty())scheme=schemes->array()->front().text("https");
        string host=getText(root,"host");string basePath=getText(root,"basePath");baseUrl=host.empty()?basePath:scheme+"://"+host+basePath;
    }
    baseUrl=resolveImportBase(baseUrl,sourceUrl);
    auto findOrCreateTagFolder=[&](const string& tag)->ApiFolder*{
        string name=trimText(tag);if(name.empty())return &target;for(auto& child:target.children)if(equalsIgnoreCase(child->name,name))return child.get();
        auto folder=std::make_unique<ApiFolder>();folder->id=newId();folder->name=name;ApiFolder* pointer=folder.get();target.children.push_back(std::move(folder));return pointer;
    };
    const char* methods[]={"get","post","put","patch","delete","head","options"};
    std::vector<string> plannedKeys;
    for(const auto& pathEntry:*paths->object()) {
        auto pathObject=pathEntry.second.object();if(!pathObject)continue;
        for(const auto& methodEntry:*pathObject) {
            auto knownMethod=std::find_if(std::begin(methods),std::end(methods),[&](const char* method){return equalsIgnoreCase(methodEntry.first,method);});if(knownMethod==std::end(methods))continue;const char* method=*knownMethod;const Json* operationPointer=resolveRef(root,&methodEntry.second);if(!operationPointer||!operationPointer->object())continue;const Json& operation=*operationPointer;
            std::vector<string> tags;if(auto tagValues=operation.get("tags");tagValues&&tagValues->array())for(const auto& tag:*tagValues->array()){string name=trimText(tag.text());if(!name.empty()&&std::none_of(tags.begin(),tags.end(),[&](const string& existing){return equalsIgnoreCase(existing,name);}))tags.push_back(std::move(name));}
            if(tags.empty())tags.push_back({});
            string upper=method;for(char& c:upper)c=(char)std::toupper((unsigned char)c);string url=combineUrl(baseUrl,pathEntry.first);
            string name=trimText(getText(operation,"summary",getText(operation,"operationId",getText(operation,"description",upper+" "+pathEntry.first))));if(name.empty())name=upper+" "+pathEntry.first;
            auto buildRequest=[&](){
                auto result=std::make_unique<ApiRequest>();result->id=newId();result->name=name;result->method=upper;result->url=url;
                auto applyParameters=[&](const Json* parameters){
                    if(!parameters||!parameters->array())return;
                    for(const auto& unresolved:*parameters->array()){
                        const Json* resolved=resolveRef(root,&unresolved);if(!resolved)continue;string where=getText(*resolved,"in"),key=getText(*resolved,"name");if(key.empty())continue;string value=parameterValue(root,*resolved),type=parameterType(root,*resolved),description=parameterDescription(root,*resolved);
                        if(equalsIgnoreCase(where,"query"))result->query.push_back({true,key,value,type,description});
                        else if(equalsIgnoreCase(where,"header"))result->headers.push_back({true,key,value,type,description});
                        else if(equalsIgnoreCase(where,"body")){result->bodyType="JSON";string sample;if(auto example=resolved->get("example"))sample=jsonText(*example);else if(auto schema=resolved->get("schema"))sample=jsonText(sampleFromSchema(root,schema));result->body=sample.empty()?"{}":sample;}
                    }
                };
                auto pathParameters=std::find_if(pathObject->begin(),pathObject->end(),[](const auto& entry){return entry.first=="parameters";});if(pathParameters!=pathObject->end())applyParameters(&pathParameters->second);applyParameters(operation.get("parameters"));
                if(openApi)if(auto requestBody=resolveRef(root,operation.get("requestBody")))if(auto content=requestBody->get("content");content&&content->object()&&!content->object()->empty()){
                    const Json* formMedia=nullptr;const Json* jsonMedia=nullptr;for(const auto& candidate:*content->object()){if(containsIgnoreCase(candidate.first,"multipart/form-data"))formMedia=&candidate.second;else if(!jsonMedia&&containsIgnoreCase(candidate.first,"application/json"))jsonMedia=&candidate.second;}
                    if(formMedia){result->bodyType="Multipart Form Data";if(const Json* media=resolveRef(root,formMedia))appendSchemaFields(root,media->get("schema"),result->formFields);}
                    else {const Json* media=jsonMedia?jsonMedia:&content->object()->begin()->second;string sample;if(bodySample(root,media,sample)){result->bodyType="JSON";result->body=sample;}}
                }
                return result;
            };
            for(const auto& tag:tags){
                ApiFolder* folder=findOrCreateTagFolder(tag);string plannedKey=folder->id+"|"+upper+"|"+url;if(std::any_of(plannedKeys.begin(),plannedKeys.end(),[&](const string& existing){return equalsIgnoreCase(existing,plannedKey);}))continue;plannedKeys.push_back(std::move(plannedKey));ApiRequest* conflict=nullptr;for(auto& existing:folder->requests)if(_stricmp(existing->method.c_str(),upper.c_str())==0&&existing->url==url){conflict=existing.get();break;}
                if(conflict&&!overwrite){++summary.skipped;continue;}auto imported=buildRequest();
                if(conflict){conflict->name=imported->name;conflict->method=imported->method;conflict->url=imported->url;conflict->query=std::move(imported->query);conflict->headers=std::move(imported->headers);conflict->bodyType=imported->bodyType;conflict->body=imported->body;conflict->formFields=std::move(imported->formFields);++summary.overwritten;}
                else{folder->requests.push_back(std::move(imported));++summary.added;}
            }
        }
    }
    return true;
}

bool isValidJson(const string& value) {
    Json parsed;return Parser(value).parse(parsed);
}

wstring jsonValidationMessage(const string& value,bool includeColumn) {
    Json parsed;Parser parser(value);if(parser.parse(parsed))return {};
    size_t position=std::min(parser.errorPosition(),value.size()),line=1,lineStart=0;
    for(size_t i=0;i<position;++i)if(value[i]=='\n'){++line;lineStart=i+1;}
    size_t column=position-lineStart+1;
    if(includeColumn)return L"JSON 格式不正确：第 "+std::to_wstring(line)+L" 行，第 "+std::to_wstring(column)+L" 个字符附近。";
    return L"JSON 格式不正确：第 "+std::to_wstring(line)+L" 行附近。";
}
