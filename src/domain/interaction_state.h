#pragma once

#include "domain/models.h"
#include <map>
#include <tuple>
#include <cwctype>

inline bool sameEntries(const std::vector<KeyValueEntry>& a, const std::vector<KeyValueEntry>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tie(a[i].enabled,a[i].key,a[i].value,a[i].type,a[i].description) !=
            std::tie(b[i].enabled,b[i].key,b[i].value,b[i].type,b[i].description)) return false;
    return true;
}

// A copyable editing baseline. IDs and child cases are deliberately not edited.
struct RequestContent {
    std::string name, method, url, bodyType, body;
    std::vector<KeyValueEntry> query, headers, formFields;
    template<class T> static RequestContent from(const T& r) {
        return {r.name,r.method,r.url,r.bodyType,r.body,r.query,r.headers,r.formFields};
    }
    template<class T> void apply(T& r) const {
        r.name=name;r.method=method;r.url=url;r.bodyType=bodyType;r.body=body;
        r.query=query;r.headers=headers;r.formFields=formFields;
    }
    bool operator==(const RequestContent& r) const {
        return std::tie(name,method,url,bodyType,body)==std::tie(r.name,r.method,r.url,r.bodyType,r.body) &&
            sameEntries(query,r.query)&&sameEntries(headers,r.headers)&&sameEntries(formFields,r.formFields);
    }
};

struct CaseContent {
    RequestContent request;
    std::string summary, raw, pretty;
    std::vector<KeyValueEntry> headers;
    static CaseContent from(const ApiRequestCase& c) {
        return {RequestContent::from(c),c.responseSummary,c.responseRaw,c.responsePretty,c.responseHeaders};
    }
    void apply(ApiRequestCase& c) const {
        request.apply(c);c.responseSummary=summary;c.responseRaw=raw;c.responsePretty=pretty;c.responseHeaders=headers;
    }
    bool operator==(const CaseContent& c) const {
        return request==c.request&&summary==c.summary&&raw==c.raw&&pretty==c.pretty&&sameEntries(headers,c.headers);
    }
};

struct WorkspaceContent {
    std::map<std::string,RequestContent> requests;
    std::map<std::string,CaseContent> cases;
    std::vector<std::string> catalog;
    void add(const ApiFolder& f) {
        catalog.insert(catalog.end(),{"folder",f.id,f.name});
        for (const auto& child:f.children) add(*child);
        for (const auto& r:f.requests) {
            catalog.insert(catalog.end(),{"request",r->id});requests.emplace(r->id,RequestContent::from(*r));
            for (const auto& c:r->cases) {catalog.insert(catalog.end(),{"case",c->id});cases.emplace(c->id,CaseContent::from(*c));}
            catalog.push_back("end-request");
        }
        catalog.push_back("end-folder");
    }
    static WorkspaceContent from(const AppData& data) {
        WorkspaceContent result;for(const auto& folder:data.folders)result.add(*folder);return result;
    }
    bool operator==(const WorkspaceContent& c) const {return requests==c.requests&&cases==c.cases&&catalog==c.catalog;}
};

struct EntryCell {int row, column;};
// A negative row means leave the grid; the final blank row never creates another blank row.
inline EntryCell nextEntryCell(int row,int column,int count,bool backwards) {
    int index=row*4+column-1+(backwards?-1:1);
    return index<0||index>=count*4?EntryCell{-1,backwards?-1:1}:EntryCell{index/4,index%4+1};
}

struct ResponseMatches {
    std::vector<size_t> positions;
    static ResponseMatches find(std::wstring content,std::wstring query) {
        ResponseMatches result;if(query.empty())return result;
        for(auto& c:content)c=static_cast<wchar_t>(towlower(c));
        for(auto& c:query)c=static_cast<wchar_t>(towlower(c));
        for(size_t pos=content.find(query);pos!=std::wstring::npos;pos=content.find(query,pos+query.size()))result.positions.push_back(pos);
        return result;
    }
};

int tabSelectionAfterClose(int selectedIndex, int closedIndex, int tabCountBeforeClose);
bool listCheckboxStateChanged(unsigned oldState, unsigned newState);
std::wstring formatBytes(size_t bytes);
