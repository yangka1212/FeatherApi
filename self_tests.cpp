#include <winsock2.h>
#include "app.h"
#include <objbase.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace {
int failures=0;
void check(bool condition,const char* name) {
    if(condition)std::cout<<"PASS  "<<name<<"\n";
    else { std::cout<<"FAIL  "<<name<<"\n";++failures; }
}

struct LoopbackExchange {
    SOCKET listener=INVALID_SOCKET;
    unsigned short port=0;
    std::string request;
    std::string responseBody=R"({"ok":true})";
    std::string responseHeaders="X-FeatherApi-Test: loopback\r\nX-Order: first\r\nX-Order: second\r\n";
    bool served=false;
    std::thread worker;

    bool start() {
        listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(listener==INVALID_SOCKET)return false;
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=0;
        if(bind(listener,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==SOCKET_ERROR||listen(listener,1)==SOCKET_ERROR)return false;
        int length=sizeof(address);if(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length)==SOCKET_ERROR)return false;port=ntohs(address.sin_port);
        worker=std::thread([this](){
            fd_set readable;FD_ZERO(&readable);FD_SET(listener,&readable);timeval timeout{5,0};
            if(select(0,&readable,nullptr,nullptr,&timeout)<=0)return;
            SOCKET client=accept(listener,nullptr,nullptr);if(client==INVALID_SOCKET)return;
            char buffer[2048];size_t expected=std::string::npos;
            while(request.size()<64*1024){
                int count=recv(client,buffer,sizeof(buffer),0);if(count<=0)break;request.append(buffer,(size_t)count);
                auto headersEnd=request.find("\r\n\r\n");if(headersEnd!=std::string::npos){
                    if(expected==std::string::npos){expected=headersEnd+4;auto contentLength=request.find("Content-Length:");if(contentLength!=std::string::npos){contentLength+=15;while(contentLength<request.size()&&request[contentLength]==' ')++contentLength;expected+=(size_t)std::stoul(request.substr(contentLength));}}
                    if(request.size()>=expected)break;
                }
            }
            const std::string response="HTTP/1.1 201 Created\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: "+std::to_string(responseBody.size())+"\r\n"+responseHeaders+"Connection: close\r\n\r\n"+responseBody;
            size_t sent=0;while(sent<response.size()){int count=send(client,response.data()+sent,(int)(response.size()-sent),0);if(count<=0)break;sent+=(size_t)count;}
            served=sent==response.size();shutdown(client,SD_BOTH);closesocket(client);
        });return true;
    }
    ~LoopbackExchange(){if(worker.joinable())worker.join();if(listener!=INVALID_SOCKET)closesocket(listener);}
};
}

int main() {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    WSADATA winsock{};const bool winsockReady=WSAStartup(MAKEWORD(2,2),&winsock)==0;

    ApiRequest original;
    original.id=newId();original.name="用户详情";original.method="POST";original.url="https://example.test/users";
    original.query.push_back({true,"名字","张三"});
    original.headers.push_back({true,"Authorization","Bearer token"});
    original.bodyType="JSON";original.body=R"({"name":"张三"})";
    auto requestCase=std::make_unique<ApiRequestCase>();requestCase->id=newId();requestCase->name="成功";original.cases.push_back(std::move(requestCase));
    auto copy=cloneRequest(original);
    check(copy->id!=original.id,"clone gives request a new id");
    check(copy->name=="用户详情 - 副本","clone uses WPF-compatible copy suffix");
    check(copy->query.size()==1&&copy->query[0].value=="张三","clone preserves Unicode params");
    check(copy->cases.size()==1&&copy->cases[0]->id!=original.cases[0]->id,"clone gives cases new ids");
    check(tabSelectionAfterClose(2,0,4)==1,"closing a tab before the selection preserves the selected tab");
    check(tabSelectionAfterClose(1,3,4)==1,"closing a tab after the selection preserves the selected tab");
    check(tabSelectionAfterClose(1,1,4)==1,"closing the selected tab selects the tab now at its position");
    check(tabSelectionAfterClose(3,3,4)==2,"closing the last selected tab selects its left neighbor");
    check(tabSelectionAfterClose(0,0,1)==-1,"closing the only tab clears the selection");
    check(listCheckboxStateChanged(0x1000,0x2000),"checking an entry is detected as a checkbox change");
    check(listCheckboxStateChanged(0x2000,0x1000),"unchecking an entry is detected as a checkbox change");
    check(!listCheckboxStateChanged(0x2002,0x2003)&&!listCheckboxStateChanged(0,0),"selection and empty state notifications do not mimic checkbox changes");
    check(formatBytes(1023)==L"1023 B"&&formatBytes(1024)==L"1 KB","response sizes use WPF byte and integer-KB formatting");
    check(formatBytes(1536)==L"1.5 KB"&&formatBytes(1290)==L"1.26 KB","response sizes omit insignificant decimal zeroes like WPF");
    check(formatBytes(1024*1024)==L"1 MB"&&formatBytes(1572864)==L"1.5 MB","response sizes use WPF MB formatting");

    auto tempRoot=std::filesystem::temp_directory_path()/std::filesystem::path(L"FeatherApi-SelfTests")/toWide(newId());
    auto dataPath=tempRoot/L"data.json";AppData stored;stored.sidebarWidth=275;stored.requestPanelHeight=360;
    auto storedFolder=std::make_unique<ApiFolder>();storedFolder->id=newId();storedFolder->name="测试目录";storedFolder->expanded=false;
    storedFolder->requests.push_back(cloneRequest(original));stored.folders.push_back(std::move(storedFolder));
    check(saveAppDataToPath(stored,dataPath.wstring()),"storage saves to an explicit path");
    AppData loaded;check(loadAppDataFromPath(dataPath.wstring(),loaded),"storage loads from an explicit path");
    check(loaded.sidebarWidth==275&&loaded.requestPanelHeight==360,"storage preserves panel settings");
    check(loaded.folders.size()==1&&!loaded.folders[0]->expanded&&loaded.folders[0]->requests[0]->name=="用户详情 - 副本","storage round-trips nested request data");
    auto normalizedPath=tempRoot/L"normalized.json";{
        std::ofstream normalized(normalizedPath,std::ios::binary);normalized<<R"({"Settings":{"SidebarWidth":999,"RequestPanelHeight":10},"Folders":[{"Id":"","Name":"   ","Children":[],"Requests":[{"Id":"","Name":" ","Method":" post ","Cases":[]}]}]})";
    }
    AppData normalized;check(loadAppDataFromPath(normalizedPath.wstring(),normalized),"storage loads WPF-compatible JSON");
    check(normalized.sidebarWidth==420&&normalized.requestPanelHeight==180,"storage clamps settings like WPF");
    check(!normalized.folders[0]->id.empty()&&normalized.folders[0]->name=="未命名目录","storage normalizes folder identity and name");
    check(!normalized.folders[0]->requests[0]->id.empty()&&normalized.folders[0]->requests[0]->name=="未命名接口"&&normalized.folders[0]->requests[0]->method=="POST","storage normalizes request identity, name and method");
    auto casingPath=tempRoot/L"storage-casing.json";{std::ofstream casingFile(casingPath,std::ios::binary);casingFile<<R"({"settings":{"sidebarwidth":300},"folders":[{"id":"folder","name":"Case","children":[],"requests":[{"id":"request","name":"Loaded","method":"get","bodytype":"","queryparams":[{"isenabled":false,"key":"q","value":"x"}],"cases":[]}]}]})";}
    AppData storageCasing;check(loadAppDataFromPath(casingPath.wstring(),storageCasing),"storage accepts property casing like WPF");
    check(storageCasing.sidebarWidth==300&&storageCasing.folders[0]->requests[0]->bodyType=="None"&&!storageCasing.folders[0]->requests[0]->query[0].enabled,"storage property casing and blank body type normalize like WPF");
    auto emptyPath=tempRoot/L"empty.json";{std::ofstream empty(emptyPath,std::ios::binary);empty<<R"({"Folders":[]})";}
    AppData empty;check(loadAppDataFromPath(emptyPath.wstring(),empty)&&empty.folders.size()==1&&empty.folders[0]->name=="默认目录","storage normalizes empty catalogs");
    std::error_code cleanupError;std::filesystem::remove_all(tempRoot,cleanupError);

    const std::string openApi=R"({
      "openapi":"3.0.1",
      "servers":[{"url":"https://api.example.com"}],
      "paths":{
        "/users":{
          "get":{"summary":"用户列表","tags":["用户"],"parameters":[
            {"in":"query","name":"page"},{"in":"header","name":"X-Tenant"}
          ]},
          "post":{"operationId":"createUser","tags":["用户"],"requestBody":{"content":{"application/json":{"schema":{"type":"object","properties":{"name":{"type":"string"}}}}}}}
        }
      }
    })";
    ApiFolder target;target.id=newId();target.name="模块";
    OpenApiImportSummary first;std::wstring error;
    check(importOpenApiJson(target,openApi,false,first,error),"OpenAPI 3 document imports");
    check(first.skippedOperations==0,"OpenAPI reports skipped-operation count");
    check(first.added==2&&target.children.size()==1&&target.children[0]->requests.size()==2,"OpenAPI tags create child folder");
    check(target.children[0]->requests[0]->query.size()==1&&target.children[0]->requests[0]->headers.size()==1,"OpenAPI parameters import");
    check(target.children[0]->requests[1]->bodyType=="JSON","OpenAPI JSON body imports");
    check(target.children[0]->requests[1]->body.find("\"name\"")!=std::string::npos,"OpenAPI schema creates JSON sample");
    OpenApiImportSummary skipped;
    check(importOpenApiJson(target,openApi,false,skipped,error)&&skipped.skipped==2,"OpenAPI conflicts can skip");
    OpenApiImportSummary overwritten;
    check(importOpenApiJson(target,openApi,true,overwritten,error)&&overwritten.overwritten==2,"OpenAPI conflicts can overwrite");

    const std::string referenced=R"({
      "openapi":"3.0.1",
      "components":{
        "parameters":{"Page":{"name":"page","in":"query","schema":{"type":"integer","default":2}}},
        "schemas":{"User":{"type":"object","properties":{"name":{"type":"string","example":"Ada"},"active":{"type":"boolean"}}}},
        "requestBodies":{"UserBody":{"content":{"application/json":{"schema":{"$ref":"#/components/schemas/User"}}}}}
      },
      "paths":{"/users":{"post":{
        "summary":"创建用户","tags":["用户","管理"],
        "parameters":[{"$ref":"#/components/parameters/Page"}],
        "requestBody":{"$ref":"#/components/requestBodies/UserBody"}
      }}}
    })";
    ApiFolder refs;refs.id=newId();refs.name="引用";OpenApiImportSummary refSummary;
    check(importOpenApiJson(refs,referenced,false,refSummary,error),"OpenAPI local refs import");
    check(refSummary.added==2&&refs.children.size()==2,"multiple tags duplicate operation into each folder");
    check(refs.children[0]->requests[0]->query[0].value=="2","parameter schema default imports");
    check(refs.children[0]->requests[0]->body.find("Ada")!=std::string::npos,"referenced schema example builds body");
    auto keptId=refs.children[0]->requests[0]->id;
    auto keptCase=std::make_unique<ApiRequestCase>();keptCase->id=newId();refs.children[0]->requests[0]->cases.push_back(std::move(keptCase));
    OpenApiImportSummary refOverwrite;
    check(importOpenApiJson(refs,referenced,true,refOverwrite,error),"referenced OpenAPI overwrites");
    check(refs.children[0]->requests[0]->id==keptId&&refs.children[0]->requests[0]->cases.size()==1,"overwrite preserves request id and cases");

    const std::string swagger=R"({
      "swagger":"2.0","schemes":["https"],"host":"legacy.example.com","basePath":"/v1",
      "paths":{"/search":{"post":{"summary":"搜索","parameters":[
        {"in":"formData","name":"keyword"},{"in":"body","name":"payload"}
      ]}}}
    })";
    ApiFolder legacy;legacy.id=newId();legacy.name="旧接口";OpenApiImportSummary legacySummary;
    check(importOpenApiJson(legacy,swagger,false,legacySummary,error),"Swagger 2 document imports");
    check(legacy.requests.size()==1&&legacy.requests[0]->url=="https://legacy.example.com/v1/search","Swagger base URL resolves");
    check(legacy.requests[0]->formFields.empty()&&legacy.requests[0]->bodyType=="JSON","Swagger formData handling matches WPF");

    const std::string swaggerDefaults=R"({
      "swagger":"2.0","host":"api.example.com","basePath":"/v2/",
      "paths":{"/orders":{"get":{"tags":[" Orders ","orders"]}}}
    })";
    ApiFolder defaults;defaults.id=newId();defaults.name="默认";OpenApiImportSummary defaultsSummary;
    check(importOpenApiJson(defaults,swaggerDefaults,false,defaultsSummary,error),"Swagger defaults import");
    check(defaults.children.size()==1&&defaults.children[0]->name=="Orders","tags are trimmed and deduplicated case-insensitively");
    check(defaults.children[0]->requests[0]->url=="https://api.example.com/v2/orders","Swagger defaults to HTTPS and combines slashes");

    const std::string relativeSwagger=R"({"swagger":"2.0","basePath":"/v1","paths":{"/health":{"get":{}}}})";
    ApiFolder relative;relative.id=newId();relative.name="相对";OpenApiImportSummary relativeSummary;
    check(importOpenApiJson(relative,relativeSwagger,false,relativeSummary,error)&&relative.requests[0]->url=="/v1/health","Swagger without host preserves basePath");

    const std::string casing=R"({"openapi":"3.0.1","paths":{"/items":{"POST":{"parameters":[{"in":"QUERY","name":"page"}],"requestBody":{"content":{"Application/JSON; charset=utf-8":{"schema":{"type":"object","properties":{"id":{"type":"integer"}}}}}}}}}})";
    ApiFolder casingTarget;casingTarget.id=newId();casingTarget.name="大小写";OpenApiImportSummary casingSummary;
    check(importOpenApiJson(casingTarget,casing,false,casingSummary,error),"OpenAPI casing variants import");
    check(casingTarget.requests[0]->method=="POST"&&casingTarget.requests[0]->query.size()==1&&casingTarget.requests[0]->bodyType=="JSON","OpenAPI method, parameter location and media type match WPF casing rules");
    const std::string noOperations=R"({"openapi":"3.0.1","paths":{"/metadata":{"parameters":[]}}})";
    ApiFolder emptyImport;emptyImport.id=newId();emptyImport.name="空文档";emptyImport.expanded=false;OpenApiImportSummary emptyImportSummary;
    check(importOpenApiJson(emptyImport,noOperations,false,emptyImportSummary,error)&&emptyImportSummary.added==0&&emptyImport.requests.empty()&&!emptyImport.expanded,"OpenAPI document without operations is a non-error like WPF");
    const std::string duplicatePlan=R"({"openapi":"3.0.1","servers":[{"url":"https://api.example.com/"}],"paths":{"/Items":{"get":{"summary":"First"}},"items":{"get":{"summary":"Second"}}}})";
    ApiFolder deduplicated;deduplicated.id=newId();deduplicated.name="去重";OpenApiImportSummary deduplicatedSummary;
    check(importOpenApiJson(deduplicated,duplicatePlan,false,deduplicatedSummary,error)&&deduplicatedSummary.added==1&&deduplicatedSummary.skipped==0&&deduplicated.requests.size()==1,"OpenAPI import plan deduplicates method and normalized URL like WPF");
    const std::string orderedPaths=R"({"openapi":"3.0.1","paths":{"/z-last":{"get":{"summary":"First in document"}},"/a-first":{"get":{"summary":"Second in document"}}}})";
    ApiFolder ordered;ordered.id=newId();ordered.name="顺序";OpenApiImportSummary orderedSummary;
    check(importOpenApiJson(ordered,orderedPaths,false,orderedSummary,error)&&ordered.requests.size()==2&&ordered.requests[0]->name=="First in document"&&ordered.requests[1]->name=="Second in document","OpenAPI preserves document path order like WPF");
    const std::string orderedMethods=R"({"openapi":"3.0.1","paths":{"/items":{"post":{"summary":"POST first"},"get":{"summary":"GET second"}}}})";
    ApiFolder methodOrder;methodOrder.id=newId();methodOrder.name="方法顺序";OpenApiImportSummary methodOrderSummary;
    check(importOpenApiJson(methodOrder,orderedMethods,false,methodOrderSummary,error)&&methodOrder.requests.size()==2&&methodOrder.requests[0]->method=="POST"&&methodOrder.requests[1]->method=="GET","OpenAPI preserves document method order like WPF");

    check(isValidJson(R"({"ok":[1,true,null]})"),"JSON validation accepts valid input");
    check(isValidJson(R"({"emoji":"\uD83D\uDE80"})"),"JSON validation accepts UTF-16 surrogate pairs");
    check(!isValidJson(R"({"emoji":"\uD83D"})"),"JSON validation rejects unpaired high surrogate");
    check(!isValidJson(R"({"bad":"\u12XZ"})"),"JSON validation rejects invalid Unicode escapes");
    check(!isValidJson(R"({"broken":})"),"JSON validation rejects invalid input");
    check(jsonValidationMessage("{\n  \"broken\": }",true).find(L"第 2 行，第")!=std::wstring::npos&&jsonValidationMessage("{\n  \"broken\": }",false).find(L"第 2 行附近")!=std::wstring::npos,"JSON validation reports WPF-style line and column context");
    check(!isValidJson("+1")&&!isValidJson(".5")&&!isValidJson("01")&&!isValidJson("1."),"JSON validation rejects non-JSON number syntax");
    check(!isValidJson("\"line\nfeed\""),"JSON validation rejects unescaped control characters");
    check(isValidJson("-12.5e+2"),"JSON validation accepts JSON number syntax");
    check(prettyJson(R"({"a":1})").find('\n')!=std::string::npos,"JSON pretty output is multiline");
    check(prettyJson(R"({"a":1})",true)==R"({"a":1})","JSON compact output is compact");
    check(prettyJson(R"({"text":"\u540C\u6B65\/ok","quote":"\u0022"})").find("同步/ok")!=std::string::npos&&prettyJson(R"({"text":"\u540C\u6B65\/ok","quote":"\u0022"})").find(R"("quote": "\"")")!=std::string::npos,"JSON formatting keeps readable Unicode and relaxed escaping like WPF");
    check(prettyJson(R"({"object":{},"array":[]})").find(R"("object": {})")!=std::string::npos&&prettyJson(R"({"object":{},"array":[]})").find(R"("array": [])")!=std::string::npos,"JSON formatting keeps empty objects and arrays compact like WPF");
    check(prettyJson("plain { text") == "plain { text","JSON pretty leaves non-JSON text unchanged");

    std::atomic<bool> cancel=false;
    RequestSnapshot queryUrl;queryUrl.url="https://example.test/items?sort=asc#top";queryUrl.query={{true," page ","1"},{true,"q","测试 空格"},{false,"ignored","x"}};
    check(buildRequestUrl(queryUrl)=="https://example.test/items?sort=asc&page=1&q=%E6%B5%8B%E8%AF%95%20%E7%A9%BA%E6%A0%BC#top","HTTP query encoding matches WPF");
    queryUrl.url="https://example.test/items?";queryUrl.query={{true,"page","1"}};check(buildRequestUrl(queryUrl)=="https://example.test/items?page=1","HTTP query avoids an extra separator after question mark");
    queryUrl.url="https://example.test/items?sort=asc&";check(buildRequestUrl(queryUrl)=="https://example.test/items?sort=asc&page=1","HTTP query avoids an extra separator after ampersand");
    std::vector<KeyValueEntry> formFields={{true," user name ","测试 空格"},{true,"symbol","a+b&c"},{false,"ignored","x"}};
    check(buildFormBody(formFields)=="user+name=%E6%B5%8B%E8%AF%95+%E7%A9%BA%E6%A0%BC&symbol=a%2Bb%26c","form URL encoding matches WPF");
    RequestSnapshot missingScheme;missingScheme.method="GET";missingScheme.url="example.test/users";
    auto missingSchemeResult=executeHttp(missingScheme,cancel);
    check(!missingSchemeResult.transportSuccess&&missingSchemeResult.errorCode==ERROR_WINHTTP_INVALID_URL&&missingSchemeResult.rawBody==toUtf8(missingSchemeResult.errorMessage)&&missingSchemeResult.prettyBody==missingSchemeResult.rawBody,"HTTP rejects URL without scheme like WPF");
    RequestSnapshot unsupportedScheme;unsupportedScheme.method="GET";unsupportedScheme.url="ftp://example.test/file";
    auto unsupportedSchemeResult=executeHttp(unsupportedScheme,cancel);
    check(!unsupportedSchemeResult.transportSuccess&&unsupportedSchemeResult.errorCode==ERROR_WINHTTP_INVALID_URL&&unsupportedSchemeResult.rawBody==toUtf8(unsupportedSchemeResult.errorMessage),"HTTP rejects non-HTTP schemes like WPF");
    cancel=true;RequestSnapshot preCancelled;preCancelled.method="GET";preCancelled.url="http://127.0.0.1:1/";auto preCancelledResult=executeHttp(preCancelled,cancel);cancel=false;
    check(preCancelledResult.cancelled&&!preCancelledResult.transportSuccess&&preCancelledResult.rawBody=="请求已取消。"&&preCancelledResult.prettyBody==preCancelledResult.rawBody,"HTTP honors cancellation before opening a connection");

    LoopbackExchange loopback;
    check(winsockReady&&loopback.start(),"HTTP loopback server starts");
    if(loopback.port!=0){
        RequestSnapshot live;live.method="POST";live.url="http://127.0.0.1:"+std::to_string(loopback.port)+"/echo#client";live.query={{true,"q","a b"}};live.bodyType="Raw";live.body="payload";live.headers={{true,"X-Request-Test","native"},{true,"X-Multi","one"},{true,"X-Multi","two"}};
        auto liveResult=executeHttp(live,cancel);
        if(loopback.worker.joinable())loopback.worker.join();
        const bool responseHeader=std::any_of(liveResult.headers.begin(),liveResult.headers.end(),[](const KeyValueEntry& header){return _stricmp(header.key.c_str(),"X-FeatherApi-Test")==0&&header.value=="loopback";});
        std::vector<std::string> orderedHeaders;for(const auto& header:liveResult.headers)if(_stricmp(header.key.c_str(),"X-Order")==0)orderedHeaders.push_back(header.value);
        check(loopback.served&&liveResult.transportSuccess&&liveResult.statusCode==201,"HTTP loopback request completes");
        check(loopback.request.find("POST /echo?q=a%20b HTTP/1.1")!=std::string::npos&&loopback.request.find("X-Request-Test: native")!=std::string::npos,"HTTP sends method, query and custom header");
        check(loopback.request.find("User-Agent:")==std::string::npos,"HTTP does not add a User-Agent when WPF would not");
        check(loopback.request.find("one")!=std::string::npos&&loopback.request.find("two")!=std::string::npos,"HTTP preserves multiple values for a request header");
        const bool hasPayload=loopback.request.size()>=7&&loopback.request.compare(loopback.request.size()-7,7,"payload")==0;
        check(loopback.request.find("Content-Type: text/plain; charset=utf-8")!=std::string::npos&&hasPayload,"HTTP sends Raw body like WPF");
        check(liveResult.rawBody==R"({"ok":true})"&&liveResult.prettyBody.find('\n')!=std::string::npos&&liveResult.sizeBytes==11&&responseHeader,"HTTP reads status, headers, size and formatted JSON body");
        check(orderedHeaders==std::vector<std::string>({"first","second"}),"HTTP preserves repeated response header order like WPF");
        closesocket(loopback.listener);loopback.listener=INVALID_SOCKET;
    }

    LoopbackExchange oversized;oversized.responseBody.assign(20*1024*1024+1,'x');oversized.responseHeaders.clear();
    check(winsockReady&&oversized.start(),"HTTP oversized-response loopback starts");
    if(oversized.port!=0){RequestSnapshot large;large.method="GET";large.url="http://127.0.0.1:"+std::to_string(oversized.port)+"/large";auto largeResult=executeHttp(large,cancel);if(oversized.worker.joinable())oversized.worker.join();
        check(largeResult.transportSuccess&&largeResult.truncated&&largeResult.rawBody.size()==20*1024*1024&&largeResult.prettyBody.find("[响应超过 20 MB，内容已截断]")!=std::string::npos,"HTTP marks a response truncated when data continues past the exact 20 MB boundary");closesocket(oversized.listener);oversized.listener=INVALID_SOCKET;}

    if(winsockReady)WSACleanup();
    CoUninitialize();
    std::cout<<(failures?"SELF TESTS FAILED":"ALL SELF TESTS PASSED")<<"\n";
    return failures?1:0;
}
