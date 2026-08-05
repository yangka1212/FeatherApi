#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>
#include <atomic>
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
    bool expanded = true;
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

struct OpenApiImportSummary {
    int added = 0;
    int overwritten = 0;
    int skipped = 0;
    int skippedOperations = 0;
};

std::wstring toWide(const std::string& value);
std::string toUtf8(const std::wstring& value);
std::string newId();
std::wstring win32DataPath();
bool loadAppData(AppData& data, std::wstring& sourcePath);
bool saveAppData(const AppData& data);
bool loadAppDataFromPath(const std::wstring& path, AppData& data);
bool saveAppDataToPath(const AppData& data, const std::wstring& path);
std::unique_ptr<ApiRequest> cloneRequest(const ApiRequest& source);
HttpResult executeHttp(const RequestSnapshot& request, std::atomic<bool>& cancel,
                       std::atomic<HINTERNET>* activeRequest = nullptr);
std::string buildRequestUrl(const RequestSnapshot& request);
std::string buildFormBody(const std::vector<KeyValueEntry>& fields);
bool buildMultipartFormBody(const std::vector<KeyValueEntry>& fields, const std::string& boundary,
                            std::string& body, std::wstring& error);
std::string prettyJson(const std::string& value, bool compact = false);
bool isValidJson(const std::string& value);
std::wstring jsonValidationMessage(const std::string& value, bool includeColumn = true);
bool importOpenApiJson(ApiFolder& target, const std::string& document, bool overwrite,
                       OpenApiImportSummary& summary, std::wstring& error,
                       const std::string& sourceUrl = {});
int tabSelectionAfterClose(int selectedIndex, int closedIndex, int tabCountBeforeClose);
bool listCheckboxStateChanged(unsigned oldState, unsigned newState);
std::wstring formatBytes(size_t bytes);

int runMainWindow(HINSTANCE instance, int showCommand);
