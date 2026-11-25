#pragma once

#include "domain/models.h"

std::string resolveEnvironmentUrl(const std::string& address,const std::string& host);
std::unique_ptr<ApiRequest> cloneRequest(const ApiRequest& source);
