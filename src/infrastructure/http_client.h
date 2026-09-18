#pragma once

#include "domain/models.h"
#include "platform/windows.h"
#include <winhttp.h>
#include <atomic>

HttpResult executeHttp(const RequestSnapshot& request, std::atomic<bool>& cancel,
                       std::atomic<HINTERNET>* activeRequest = nullptr);
std::string buildRequestUrl(const RequestSnapshot& request);
std::string buildFormBody(const std::vector<KeyValueEntry>& fields);
bool buildMultipartFormBody(const std::vector<KeyValueEntry>& fields, const std::string& boundary,
                            std::string& body, std::wstring& error);
