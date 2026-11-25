#include "application/request_operations.h"
#include "platform/identity.h"

#include "domain/normalization.h"

std::string resolveEnvironmentUrl(const std::string& address,const std::string& host) {
    auto url=trimText(address);
    if(host.empty()||url.empty())return url;
    auto scheme=url.find("://");
    if(scheme!=std::string::npos&&scheme==url.find_first_of(":/?#")){
        auto protocol=url.substr(0,scheme);
        if(!equalsIgnoreCase(protocol,"http")&&!equalsIgnoreCase(protocol,"https"))return {};
        auto end=url.find_first_of("/?#",scheme+3);
        if(end==scheme+3||url.size()==scheme+3)return {};
        url=end==std::string::npos?"/":url.substr(end);
    }
    return host+(url.front()=='/'?"":"/")+url;
}

std::unique_ptr<ApiRequest> cloneRequest(const ApiRequest& source) {
    auto result=std::make_unique<ApiRequest>();
    result->id=newId();result->name=source.name+" - 副本";result->method=source.method;result->url=source.url;
    result->query=source.query;result->headers=source.headers;result->bodyType=source.bodyType;result->body=source.body;result->formFields=source.formFields;
    for(const auto& item:source.cases){auto copy=std::make_unique<ApiRequestCase>(*item);copy->id=newId();result->cases.push_back(std::move(copy));}
    return result;
}
