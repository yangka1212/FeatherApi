#pragma once

#include "domain/models.h"

std::unique_ptr<ApiRequest> cloneRequest(const ApiRequest& source);
