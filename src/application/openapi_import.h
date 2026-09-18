#pragma once

#include "domain/models.h"

struct OpenApiImportSummary {
    int added = 0;
    int overwritten = 0;
    int skipped = 0;
    int skippedOperations = 0;
};

bool importOpenApiJson(ApiFolder& target, const std::string& document, bool overwrite,
                       OpenApiImportSummary& summary, std::wstring& error,
                       const std::string& sourceUrl = {});
