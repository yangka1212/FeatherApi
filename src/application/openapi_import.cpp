#include "application/openapi_import.h"
#include "domain/normalization.h"
#include "json/detail/value.h"
#include "platform/identity.h"

using namespace feather::json_detail;
namespace {
string getText(const Json& object, const char* name, const string& fallback={}) { auto p=object.get(name);return p?p->text(fallback):fallback; }
string combineUrl(string baseUrl,string path){if(baseUrl.empty())return path;while(!baseUrl.empty()&&baseUrl.back()=='/')baseUrl.pop_back();size_t start=0;while(start<path.size()&&path[start]=='/')++start;return baseUrl+"/"+path.substr(start);}
string sourceOrigin(const string& url){size_t scheme=url.find("://");if(scheme==string::npos)return {};size_t authority=url.find_first_of("/?#",scheme+3);return url.substr(0,authority==string::npos?url.size():authority);}
string resolveImportBase(string baseUrl,const string& sourceUrl){
    baseUrl=trimText(baseUrl);string origin=sourceOrigin(sourceUrl);
    if(baseUrl.empty())return origin;
    if(baseUrl.rfind("//",0)==0){size_t scheme=sourceUrl.find("://");return scheme==string::npos?baseUrl:sourceUrl.substr(0,scheme)+":"+baseUrl;}
    if(baseUrl.find("://")!=string::npos)return baseUrl;
    return combineUrl(origin,baseUrl);
}
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
