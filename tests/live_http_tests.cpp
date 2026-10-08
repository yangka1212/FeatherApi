#include "application/openapi_import.h"
#include "infrastructure/http_client.h"
#include "json/detail/value.h"
#include "platform/encoding.h"
#include "platform/identity.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

namespace {
using feather::json_detail::Json;
using feather::json_detail::Parser;

int failures = 0;

void check(bool condition, const char* description, const HttpResult* response = nullptr) {
    std::cout << (condition ? "PASS  " : "FAIL  ") << description << '\n';
    if (condition) return;
    ++failures;
    if (!response) return;
    std::cout << "      transport=" << response->transportSuccess
              << " status=" << response->statusCode
              << " error=" << response->errorCode
              << " truncated=" << response->truncated << '\n';
    if (!response->errorMessage.empty())
        std::cout << "      " << toUtf8(response->errorMessage) << '\n';
    if (!response->rawBody.empty())
        std::cout << "      body: " << response->rawBody.substr(0, 400) << '\n';
}

const Json* field(const Json* object, const char* name) {
    return object ? object->getInsensitive(name) : nullptr;
}

bool equalsText(const Json* object, const char* name, const std::string& expected) {
    const Json* value = field(object, name);
    return value && value->text() == expected;
}

bool readOkJson(const HttpResult& response, Json& document) {
    return response.transportSuccess && response.statusCode == 200 && !response.truncated &&
           Parser(response.rawBody).parse(document) && document.object();
}

struct TemporaryFile {
    std::filesystem::path path;
    ~TemporaryFile() {
        if (path.empty()) return;
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

void testGet(const std::string& baseUrl, const std::string& marker, std::atomic<bool>& cancel) {
    RequestSnapshot request;
    request.method = "GET";
    request.url = baseUrl + "/get";
    request.bodyType = "None";
    request.query = {{true, "probe", marker}, {true, "space", "two words"},
                     {false, "disabled", "must-not-send"}};
    const HttpResult response = executeHttp(request, cancel);
    Json document;
    const bool valid = readOkJson(response, document);
    check(valid, "real GET /get returns HTTP 200 JSON", &response);
    if (!valid) return;
    const Json* args = field(&document, "args");
    check(equalsText(args, "probe", marker) && equalsText(args, "space", "two words") &&
          !field(args, "disabled"), "GET query is echoed with encoding and disabled fields omitted");
}

void testJsonPost(const std::string& baseUrl, const std::string& marker, std::atomic<bool>& cancel) {
    RequestSnapshot request;
    request.method = "POST";
    request.url = baseUrl + "/post";
    request.bodyType = "JSON";
    request.body = "{\"probe\":\"" + marker + "\",\"kind\":\"real-network\"}";
    request.headers = {{true, "X-FeatherApi-Live-Test", marker}};
    const HttpResult response = executeHttp(request, cancel);
    Json document;
    const bool valid = readOkJson(response, document);
    check(valid, "real POST /post accepts JSON", &response);
    if (!valid) return;
    check(equalsText(field(&document, "json"), "probe", marker) &&
          equalsText(field(&document, "json"), "kind", "real-network"),
          "server received the exact JSON fields");
    const Json* headers = field(&document, "headers");
    const Json* contentType = field(headers, "Content-Type");
    check(equalsText(headers, "X-FeatherApi-Live-Test", marker) && contentType &&
          contentType->text().find("application/json") == 0,
          "server received the custom header and JSON content type");
}

void testFormPost(const std::string& baseUrl, const std::string& marker, std::atomic<bool>& cancel) {
    RequestSnapshot request;
    request.method = "POST";
    request.url = baseUrl + "/post";
    request.bodyType = "Form URL Encoded";
    request.formFields = {{true, "probe", marker}, {true, "message", "two words + sign"},
                          {false, "disabled", "must-not-send"}};
    const HttpResult response = executeHttp(request, cancel);
    Json document;
    const bool valid = readOkJson(response, document);
    check(valid, "real POST /post accepts URL encoded form", &response);
    if (!valid) return;
    const Json* form = field(&document, "form");
    check(equalsText(form, "probe", marker) &&
          equalsText(form, "message", "two words + sign") && !field(form, "disabled"),
          "server received decoded form fields and omitted the disabled field");
}

void testMultipartPost(const std::string& baseUrl, const std::string& marker,
                       std::atomic<bool>& cancel) {
    TemporaryFile upload;
    std::error_code error;
    const auto tempDirectory = std::filesystem::temp_directory_path(error);
    if (!error) upload.path = tempDirectory / ("FeatherApiLive-" + marker + ".txt");
    const std::string contents = "FeatherApi live upload " + marker + "\n";
    bool fileReady = false;
    if (!upload.path.empty()) {
        std::ofstream file(upload.path, std::ios::binary);
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        file.close();
        fileReady = file.good();
    }
    check(fileReady, "real multipart upload file is created on disk");
    if (!fileReady) return;

    RequestSnapshot request;
    request.method = "POST";
    request.url = baseUrl + "/post";
    request.bodyType = "Multipart Form Data";
    request.formFields = {{true, "upload", toUtf8(upload.path.wstring()), "file", ""},
                          {true, "probe", marker, "string", ""}};
    const HttpResult response = executeHttp(request, cancel);
    Json document;
    const bool valid = readOkJson(response, document);
    check(valid, "real POST /post accepts multipart file upload", &response);
    if (!valid) return;
    check(equalsText(field(&document, "files"), "upload", contents) &&
          equalsText(field(&document, "form"), "probe", marker),
          "server received the bytes from the real file and the text field");
}

void testOtherWriteMethods(const std::string& baseUrl, const std::string& marker,
                           std::atomic<bool>& cancel) {
    for (const std::string method : {"PUT", "PATCH", "DELETE"}) {
        RequestSnapshot request;
        request.method = method;
        request.url = baseUrl + "/" + std::string(method == "PUT" ? "put" :
                                                   method == "PATCH" ? "patch" : "delete");
        request.bodyType = "JSON";
        request.body = "{\"probe\":\"" + marker + "\"}";
        const HttpResult response = executeHttp(request, cancel);
        Json document;
        const bool valid = readOkJson(response, document);
        check(valid, ("real " + method + " reaches the remote API").c_str(), &response);
        if (valid) check(equalsText(field(&document, "json"), "probe", marker),
                         ("remote API received the " + method + " JSON body").c_str());
    }
}

void testHttpError(const std::string& baseUrl, std::atomic<bool>& cancel) {
    RequestSnapshot request;
    request.method = "GET";
    request.url = baseUrl + "/status/404";
    request.bodyType = "None";
    const HttpResult response = executeHttp(request, cancel);
    check(response.transportSuccess && response.statusCode == 404,
          "real HTTP 404 is reported as a response rather than a transport failure", &response);
}

void testRedirect(const std::string& baseUrl, std::atomic<bool>& cancel) {
    RequestSnapshot request;
    request.method = "GET";
    request.url = baseUrl + "/redirect/1";
    request.bodyType = "None";
    const HttpResult response = executeHttp(request, cancel);
    Json document;
    const bool valid = readOkJson(response, document);
    check(valid && response.finalUrl.find("/get") != std::string::npos,
          "real redirect follows to /get and reports final URL", &response);
}

void testRealOpenApi(const std::string& documentUrl, std::atomic<bool>& cancel) {
    RequestSnapshot request;
    request.method = "GET";
    request.url = documentUrl;
    request.bodyType = "None";
    const HttpResult response = executeHttp(request, cancel);
    const bool downloaded = response.transportSuccess && response.statusCode == 200 &&
                            !response.truncated && !response.rawBody.empty();
    check(downloaded, "real OpenAPI document downloads within the 5 MB limit", &response);
    if (!downloaded) return;

    ApiFolder imported;
    imported.id = newId();
    imported.name = "Live OpenAPI";
    OpenApiImportSummary summary;
    std::wstring error;
    const std::string source = response.finalUrl.empty() ? documentUrl : response.finalUrl;
    const bool valid = importOpenApiJson(imported, response.rawBody, false, summary, error, source);
    check(valid && summary.added > 0, "production importer maps real API operations", &response);
    if (!valid) std::cout << "      import error: " << toUtf8(error) << '\n';
}
} // namespace

int main(int argc, char* argv[]) {
    std::string baseUrl = "https://httpbin.org";
    std::string openApiUrl = "https://raw.githubusercontent.com/openai/openai-openapi/main/openapi.json";
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") {
            std::cout << "Usage: FeatherApiLiveHttpTests [--base-url HTTPBIN_COMPATIBLE_URL]"
                         " [--openapi-url DOCUMENT_URL | --skip-openapi]\n";
            return 0;
        }
        if (argument == "--base-url" && index + 1 < argc) {
            baseUrl = argv[++index];
        } else if (argument == "--openapi-url" && index + 1 < argc) {
            openApiUrl = argv[++index];
        } else if (argument == "--skip-openapi") {
            openApiUrl.clear();
        } else {
            std::cerr << "Unknown or incomplete argument: " << argument << '\n';
            return 2;
        }
    }
    while (!baseUrl.empty() && baseUrl.back() == '/') baseUrl.pop_back();
    if ((baseUrl.rfind("https://", 0) != 0 && baseUrl.rfind("http://", 0) != 0) ||
        baseUrl.find_first_of("?#") != std::string::npos) {
        std::cerr << "--base-url must be an absolute HTTP(S) URL without query or fragment\n";
        return 2;
    }

    std::cout.setf(std::ios::unitbuf);
    std::cout << "Live HTTP endpoint: " << baseUrl << '\n';
    std::atomic<bool> cancel{false};
    const std::string marker = newId();
    testGet(baseUrl, marker, cancel);
    testJsonPost(baseUrl, marker, cancel);
    testFormPost(baseUrl, marker, cancel);
    testMultipartPost(baseUrl, marker, cancel);
    testOtherWriteMethods(baseUrl, marker, cancel);
    testHttpError(baseUrl, cancel);
    testRedirect(baseUrl, cancel);
    if (!openApiUrl.empty()) testRealOpenApi(openApiUrl, cancel);
    else std::cout << "SKIP  real OpenAPI import (--skip-openapi)\n";
    std::cout << failures << " live HTTP failure(s)\n";
    return failures ? 1 : 0;
}
