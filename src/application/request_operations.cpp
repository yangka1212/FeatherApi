#include "application/request_operations.h"
#include "platform/identity.h"

std::unique_ptr<ApiRequest> cloneRequest(const ApiRequest& source) {
    auto result=std::make_unique<ApiRequest>();
    result->id=newId();result->name=source.name+" - 副本";result->method=source.method;result->url=source.url;
    result->query=source.query;result->headers=source.headers;result->bodyType=source.bodyType;result->body=source.body;result->formFields=source.formFields;
    for(const auto& item:source.cases){auto copy=std::make_unique<ApiRequestCase>(*item);copy->id=newId();result->cases.push_back(std::move(copy));}
    return result;
}
