#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct KeyValueEntry {
    bool enabled = true;
    std::string key;
    std::string value;
    std::string type;
    std::string description;
};

struct ApiRequestCase {
    std::string id;
    std::string name = "请求用例";
    std::string method = "GET";
    std::string url;
    std::vector<KeyValueEntry> query;
    std::vector<KeyValueEntry> headers;
    std::string bodyType = "None";
    std::string body;
    std::vector<KeyValueEntry> formFields;
    std::string responseSummary = "暂无响应";
    std::string responseRaw;
    std::string responsePretty;
    std::vector<KeyValueEntry> responseHeaders;
};

struct ApiRequest {
    std::string id;
    std::string name = "新接口";
    std::string method = "GET";
    std::string url;
    std::vector<KeyValueEntry> query;
    std::vector<KeyValueEntry> headers;
    std::string bodyType = "None";
    std::string body;
    std::vector<KeyValueEntry> formFields;
    std::vector<std::unique_ptr<ApiRequestCase>> cases;
};

struct ApiFolder {
    std::string id;
    std::string name = "新目录";
    bool expanded = false;
    std::vector<std::unique_ptr<ApiFolder>> children;
    std::vector<std::unique_ptr<ApiRequest>> requests;
};

struct AppData {
    int sidebarWidth = 320;
    int requestPanelHeight = 330;
    std::vector<std::unique_ptr<ApiFolder>> folders;
};

struct RequestSnapshot {
    std::string method;
    std::string url;
    std::vector<KeyValueEntry> query;
    std::vector<KeyValueEntry> headers;
    std::string bodyType;
    std::string body;
    std::vector<KeyValueEntry> formFields;
};

struct HttpResult {
    bool transportSuccess = false;
    bool cancelled = false;
    std::string finalUrl;
    unsigned statusCode = 0;
    std::wstring statusText;
    unsigned long errorCode = 0;
    long long durationMs = 0;
    size_t sizeBytes = 0;
    bool truncated = false;
    std::string rawBody;
    std::string prettyBody;
    std::vector<KeyValueEntry> headers;
    std::wstring errorMessage;
};
