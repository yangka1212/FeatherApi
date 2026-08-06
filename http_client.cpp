#include "app.h"

#include <chrono>
#include <cctype>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <fstream>

using std::string;
using std::wstring;

namespace {
string trimAscii(string value) {
    while(!value.empty()&&std::isspace((unsigned char)value.front()))value.erase(value.begin());
    while(!value.empty()&&std::isspace((unsigned char)value.back()))value.pop_back();return value;
}
string urlEncode(const string& value) {
    static const char hex[]="0123456789ABCDEF"; string result;
    for(unsigned char c:value) {
        if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')result+=(char)c;
        else { result+='%';result+=hex[c>>4];result+=hex[c&15]; }
    }
    return result;
}
string formEncode(const string& value){string result=urlEncode(value);size_t position=0;while((position=result.find("%20",position))!=string::npos){result.replace(position,3,"+");++position;}return result;}

string normalizedJsonString(const string& value,size_t& position) {
    string result;result+='"';++position;
    auto appendCodePoint=[&](unsigned code){
        if(code=='"'){result+="\\\"";return;}if(code=='\\'){result+="\\\\";return;}
        if(code<0x20){char escaped[7]{};snprintf(escaped,sizeof(escaped),"\\u%04X",code);result+=escaped;return;}
        wstring wide;if(code<=0xFFFF)wide+=(wchar_t)code;else{code-=0x10000;wide+=(wchar_t)(0xD800+(code>>10));wide+=(wchar_t)(0xDC00+(code&0x3FF));}result+=toUtf8(wide);
    };
    auto hex=[](char c)->unsigned{return c>='0'&&c<='9'?(unsigned)(c-'0'):c>='a'&&c<='f'?(unsigned)(c-'a'+10):(unsigned)(c-'A'+10);};
    while(position<value.size()){
        char c=value[position++];if(c=='"'){result+='"';break;}if(c!='\\'){result+=c;continue;}
        char escaped=value[position++];if(escaped=='u'){
            unsigned code=0;for(int i=0;i<4;++i)code=(code<<4)+hex(value[position++]);
            if(code>=0xD800&&code<=0xDBFF){position+=2;unsigned low=0;for(int i=0;i<4;++i)low=(low<<4)+hex(value[position++]);code=0x10000+((code-0xD800)<<10)+(low-0xDC00);}
            appendCodePoint(code);
        }else if(escaped=='/'){result+='/';}
        else{result+='\\';result+=escaped;}
    }return result;
}

wstring errorText(DWORD code) {
    wchar_t* message=nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr,code,0,(LPWSTR)&message,0,nullptr);
    wstring result=message?message:L"未知网络错误";
    if(message)LocalFree(message);
    while(!result.empty()&&(result.back()==L'\r'||result.back()==L'\n'))result.pop_back();
    return result;
}

wstring normalizedHttpError(DWORD code) {
    switch(code){
    case ERROR_WINHTTP_INVALID_URL:return L"请求地址无效，请输入以 http:// 或 https:// 开头的完整地址。";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:return L"无法解析服务器地址，请检查域名是否填写正确。";
    case ERROR_WINHTTP_CANNOT_CONNECT:return L"服务器拒绝连接，请检查接口地址或目标服务是否已启动。";
    case ERROR_WINHTTP_CONNECTION_ERROR:return L"无法连接到服务器，请检查网络连接。";
    case ERROR_WINHTTP_TIMEOUT:return L"连接服务器超时。";
    case ERROR_WINHTTP_SECURE_FAILURE:return L"安全连接失败，目标服务器的 HTTPS 证书可能无效或不受信任。";
    default:return L"请求连接失败："+errorText(code);
    }
}

string headerValue(const std::vector<KeyValueEntry>& headers,const string& key) {
    for(const auto& item:headers)if(_stricmp(item.key.c_str(),key.c_str())==0)return item.value;
    return {};
}

bool isJsonContentType(string contentType) {
    auto parameter=contentType.find(';');if(parameter!=string::npos)contentType.resize(parameter);
    contentType=trimAscii(contentType);for(char& c:contentType)c=(char)std::tolower((unsigned char)c);
    auto slash=contentType.find('/');if(slash==string::npos)return false;
    string subtype=trimAscii(contentType.substr(slash+1));
    return subtype=="json"||(subtype.size()>5&&subtype.compare(subtype.size()-5,5,"+json")==0);
}

string formatResponseBody(const string& body,const std::vector<KeyValueEntry>& headers) {
    if(body.empty())return {};
    return isJsonContentType(headerValue(headers,"Content-Type"))?prettyJson(body):body;
}

wstring decodeBody(const string& bytes,const std::vector<KeyValueEntry>& headers) {
    string contentType=headerValue(headers,"Content-Type"),lowerType=contentType,charset;for(char& c:lowerType)c=(char)std::tolower((unsigned char)c);
    auto position=lowerType.find("charset");if(position!=string::npos){position=lowerType.find('=',position+7);if(position!=string::npos){charset=contentType.substr(position+1);auto end=charset.find(';');if(end!=string::npos)charset.resize(end);charset=trimAscii(charset);while(charset.size()>=2&&charset.front()=='"'&&charset.back()=='"'){charset.erase(charset.begin());charset.pop_back();}}}for(char& c:charset)c=(char)std::tolower((unsigned char)c);
    if(charset=="utf-16"||charset=="utf-16le"||charset=="unicode"){
        if(bytes.empty())return {};size_t start=bytes.size()>=2&&(unsigned char)bytes[0]==0xFF&&(unsigned char)bytes[1]==0xFE?2:0;wstring result;result.reserve((bytes.size()-start)/2);for(size_t i=start;i+1<bytes.size();i+=2)result+=(wchar_t)((unsigned char)bytes[i]|((unsigned char)bytes[i+1]<<8));return result;
    }
    if(charset=="utf-16be"){
        size_t start=bytes.size()>=2&&(unsigned char)bytes[0]==0xFE&&(unsigned char)bytes[1]==0xFF?2:0;wstring result;result.reserve((bytes.size()-start)/2);for(size_t i=start;i+1<bytes.size();i+=2)result+=(wchar_t)(((unsigned char)bytes[i]<<8)|(unsigned char)bytes[i+1]);return result;
    }
    UINT codePage=CP_UTF8;
    if(charset=="gbk"||charset=="gb2312")codePage=936;else if(charset=="gb18030")codePage=54936;
    else if(charset=="big5")codePage=950;else if(charset=="shift_jis"||charset=="shift-jis"||charset=="windows-31j")codePage=932;
    else if(charset=="windows-1252")codePage=1252;else if(charset=="iso-8859-1"||charset=="latin1")codePage=28591;else if(charset=="us-ascii")codePage=20127;
    if(bytes.empty())return {};
    int count=MultiByteToWideChar(codePage,codePage==CP_UTF8?MB_ERR_INVALID_CHARS:0,bytes.data(),(int)bytes.size(),nullptr,0);
    if(!count&&codePage==CP_UTF8)count=MultiByteToWideChar(CP_UTF8,0,bytes.data(),(int)bytes.size(),nullptr,0);
    if(!count)return toWide(bytes);wstring result((size_t)count,L'\0');MultiByteToWideChar(codePage,0,bytes.data(),(int)bytes.size(),result.data(),count);return result;
}

std::vector<KeyValueEntry> parseHeaders(const wstring& raw) {
    std::vector<KeyValueEntry> result;std::wistringstream input(raw);wstring line;
    std::getline(input,line);
    while(std::getline(input,line)) {
        if(!line.empty()&&line.back()==L'\r')line.pop_back();
        auto colon=line.find(L':');if(colon==wstring::npos)continue;
        auto value=line.substr(colon+1);while(!value.empty()&&value.front()==L' ')value.erase(value.begin());
        result.push_back({true,toUtf8(line.substr(0,colon)),toUtf8(value)});
    }
    return result;
}
}

string buildRequestUrl(const RequestSnapshot& input) {
    string result=trimAscii(input.url),fragment;auto fragmentAt=result.find('#');if(fragmentAt!=string::npos){fragment=result.substr(fragmentAt);result.resize(fragmentAt);}
    for(const auto& entry:input.query)if(entry.enabled&&!trimAscii(entry.key).empty()){
        if(result.find('?')==string::npos)result+='?';else if(!result.empty()&&result.back()!='?'&&result.back()!='&')result+='&';
        result+=urlEncode(trimAscii(entry.key))+"="+urlEncode(entry.value);
    }return result+fragment;
}
string buildFormBody(const std::vector<KeyValueEntry>& fields) {
    string body;for(const auto& entry:fields)if(entry.enabled&&!trimAscii(entry.key).empty()){if(!body.empty())body+='&';body+=formEncode(trimAscii(entry.key))+"="+formEncode(entry.value);}return body;
}
bool buildMultipartFormBody(const std::vector<KeyValueEntry>& fields,const string& boundary,string& body,wstring& error) {
    body.clear();error.clear();auto quoted=[](string value){size_t at=0;while((at=value.find_first_of("\\\"\r\n",at))!=string::npos){if(value[at]=='\r'||value[at]=='\n')value.erase(at,1);else{value.insert(at,"\\");at+=2;}}return value;};
    for(const auto& entry:fields)if(entry.enabled&&!trimAscii(entry.key).empty()){
        string name=quoted(trimAscii(entry.key));body+="--"+boundary+"\r\nContent-Disposition: form-data; name=\""+name+"\"";
        if(_stricmp(entry.type.c_str(),"file")==0){std::filesystem::path path=std::filesystem::u8path(entry.value);std::ifstream input(path,std::ios::binary);if(!input){error=L"无法读取表单文件："+path.wstring();body.clear();return false;}std::error_code ec;auto size=std::filesystem::file_size(path,ec);if(ec||size>100ull*1024*1024){error=L"表单文件无效或超过 100 MB："+path.wstring();body.clear();return false;}string bytes((std::istreambuf_iterator<char>(input)),{});string filename=quoted(toUtf8(path.filename().wstring()));body+="; filename=\""+filename+"\"\r\nContent-Type: application/octet-stream\r\n\r\n";body+=bytes;body+="\r\n";}
        else {body+="\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n"+entry.value+"\r\n";}
    }
    body+="--"+boundary+"--\r\n";return true;
}

string prettyJson(const string& value, bool compact) {
    if(!isValidJson(value))return value;
    string output;output.reserve(value.size()+128);
    int indent=0;bool quoted=false,escaped=false;
    auto newline=[&](){if(compact)return;output+='\n';output.append((size_t)indent*2,' ');};
    for(size_t position=0;position<value.size();) {
        char c=value[position++];
        if(quoted) {
            output+=c;
            if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;
            continue;
        }
        if(c=='"'){--position;output+=normalizedJsonString(value,position);}
        else if(c=='{'||c=='['){size_t next=position;while(next<value.size()&&std::isspace((unsigned char)value[next]))++next;char close=c=='{'?'}':']';if(next<value.size()&&value[next]==close){output+=c;output+=close;position=next+1;}else{output+=c;++indent;newline();}}
        else if(c=='}'||c==']'){--indent;newline();output+=c;}
        else if(c==','){output+=c;newline();}
        else if(c==':'){output+=c;if(!compact)output+=' ';}
        else if(!std::isspace((unsigned char)c))output+=c;
    }
    return output.empty()?value:output;
}

HttpResult executeHttp(const RequestSnapshot& input,std::atomic<bool>& cancel,std::atomic<HINTERNET>* activeRequest) {
    HttpResult result;auto started=std::chrono::steady_clock::now();
    auto setFailureBody=[&](const wstring& message){result.errorMessage=message;result.rawBody=toUtf8(message);result.prettyBody=result.rawBody;};
    if(cancel.load()){result.cancelled=true;setFailureBody(L"请求已取消。");return result;}
    string rawUrl=buildRequestUrl(input);
    wstring url=toWide(rawUrl);
    URL_COMPONENTSW parts{};parts.dwStructSize=sizeof(parts);
    wchar_t host[1024]{},path[8192]{},extra[8192]{};
    parts.lpszHostName=host;parts.dwHostNameLength=1023;
    parts.lpszUrlPath=path;parts.dwUrlPathLength=8191;
    parts.lpszExtraInfo=extra;parts.dwExtraInfoLength=8191;
    if(!WinHttpCrackUrl(url.c_str(),0,0,&parts)||(parts.nScheme!=INTERNET_SCHEME_HTTP&&parts.nScheme!=INTERNET_SCHEME_HTTPS)) {
        result.errorCode=ERROR_WINHTTP_INVALID_URL;setFailureBody(L"请求地址无效，请输入以 http:// 或 https:// 开头的完整地址。");return result;
    }
    wstring object=path[0]?path:L"/";wstring suffix=extra;
    auto fragmentAt=suffix.find(L'#');if(fragmentAt!=wstring::npos)suffix.resize(fragmentAt);
    object+=suffix;
    string body=input.body;string multipartBoundary;
    if(input.bodyType=="Form URL Encoded") {
        body=buildFormBody(input.formFields);
    } else if(input.bodyType=="Multipart Form Data") {
        multipartBoundary="----FeatherApi"+newId();wstring multipartError;if(!buildMultipartFormBody(input.formFields,multipartBoundary,body,multipartError)){result.errorCode=ERROR_FILE_NOT_FOUND;setFailureBody(multipartError);result.durationMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();return result;}
    } else if(input.bodyType=="None") body.clear();
    wstring headers;
    bool hasContentType=false;
    for(const auto& entry:input.headers)if(entry.enabled&&!trimAscii(entry.key).empty()) {
        string key=trimAscii(entry.key),lower=key;for(char& c:lower)c=(char)std::tolower((unsigned char)c);if(input.bodyType=="None"&&(lower=="content-type"||lower=="content-length"))continue;
        headers+=toWide(key)+L": "+toWide(entry.value)+L"\r\n";if(lower=="content-type")hasContentType=true;
    }
    if(!hasContentType&&input.bodyType=="JSON")headers+=L"Content-Type: application/json; charset=utf-8\r\n";
    if(!hasContentType&&input.bodyType=="Form URL Encoded")headers+=L"Content-Type: application/x-www-form-urlencoded\r\n";
    if(!hasContentType&&input.bodyType=="Multipart Form Data")headers+=L"Content-Type: multipart/form-data; boundary="+toWide(multipartBoundary)+L"\r\n";
    if(!hasContentType&&input.bodyType=="Raw")headers+=L"Content-Type: text/plain; charset=utf-8\r\n";

    HINTERNET session=WinHttpOpen(L"",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    HINTERNET connection=session?WinHttpConnect(session,host,parts.nPort,0):nullptr;
    HINTERNET request=connection?WinHttpOpenRequest(connection,toWide(input.method).c_str(),object.c_str(),nullptr,
        WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0):nullptr;
    if(activeRequest)activeRequest->store(request);
    if(request) {
        WinHttpSetTimeouts(request,0,0,0,0);
        DWORD policy=WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&policy,sizeof(policy));
        if(!headers.empty())WinHttpAddRequestHeaders(request,headers.c_str(),(DWORD)-1,WINHTTP_ADDREQ_FLAG_ADD);
    }
    BOOL ok=!cancel.load()&&request&&WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,
        body.empty()?WINHTTP_NO_REQUEST_DATA:(void*)body.data(),(DWORD)body.size(),(DWORD)body.size(),0);
    if(ok)ok=WinHttpReceiveResponse(request,nullptr);
    if(ok) {
        DWORD size=sizeof(result.statusCode);
        WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,
                            &result.statusCode,&size,WINHTTP_NO_HEADER_INDEX);
        DWORD statusTextSize=0;WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_TEXT,WINHTTP_HEADER_NAME_BY_INDEX,nullptr,&statusTextSize,WINHTTP_NO_HEADER_INDEX);
        if(GetLastError()==ERROR_INSUFFICIENT_BUFFER&&statusTextSize){wstring text(statusTextSize/sizeof(wchar_t),L'\0');if(WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_TEXT,WINHTTP_HEADER_NAME_BY_INDEX,text.data(),&statusTextSize,WINHTTP_NO_HEADER_INDEX)){while(!text.empty()&&!text.back())text.pop_back();result.statusText=std::move(text);}}
        DWORD headerSize=0;WinHttpQueryHeaders(request,WINHTTP_QUERY_RAW_HEADERS_CRLF,WINHTTP_HEADER_NAME_BY_INDEX,nullptr,&headerSize,WINHTTP_NO_HEADER_INDEX);
        if(GetLastError()==ERROR_INSUFFICIENT_BUFFER&&headerSize) {
            wstring rawHeaders(headerSize/sizeof(wchar_t),L'\0');
            if(WinHttpQueryHeaders(request,WINHTTP_QUERY_RAW_HEADERS_CRLF,WINHTTP_HEADER_NAME_BY_INDEX,rawHeaders.data(),&headerSize,WINHTTP_NO_HEADER_INDEX))
                result.headers=parseHeaders(rawHeaders);
        }
        std::stable_sort(result.headers.begin(),result.headers.end(),[](const KeyValueEntry& left,const KeyValueEntry& right){return _stricmp(left.key.c_str(),right.key.c_str())<0;});
        char buffer[8192];DWORD read=0;
        while(!cancel) {
            if(!WinHttpReadData(request,buffer,sizeof(buffer),&read)){ok=FALSE;break;}
            if(!read)break;size_t remaining=20*1024*1024-result.rawBody.size();if(!remaining){result.truncated=true;break;}size_t take=std::min<size_t>(read,remaining);result.rawBody.append(buffer,take);if(take<read){result.truncated=true;break;}
        }
    }
    result.cancelled=cancel.load();
    result.transportSuccess=ok&&!result.cancelled;
    if(result.cancelled)setFailureBody(L"请求已取消。");
    else if(!result.transportSuccess) {
        result.errorCode=GetLastError();
        setFailureBody(normalizedHttpError(result.errorCode));
    }
    if(result.transportSuccess){
        result.sizeBytes=result.rawBody.size();string contentLength=headerValue(result.headers,"Content-Length");if(!contentLength.empty())try{result.sizeBytes=(size_t)std::stoull(trimAscii(contentLength));}catch(const std::exception&){}
        result.rawBody=toUtf8(decodeBody(result.rawBody,result.headers));
        result.prettyBody=result.rawBody.size()<=2*1024*1024?formatResponseBody(result.rawBody,result.headers):result.rawBody;
        if(result.truncated)result.prettyBody+="\r\n\r\n[响应超过 20 MB，内容已截断]";
    }
    result.durationMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
    if(activeRequest){HINTERNET owned=activeRequest->exchange(nullptr);if(owned)WinHttpCloseHandle(owned);}
    else if(request)WinHttpCloseHandle(request);
    if(connection)WinHttpCloseHandle(connection);if(session)WinHttpCloseHandle(session);
    return result;
}
