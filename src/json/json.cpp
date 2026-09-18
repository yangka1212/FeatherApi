#include "json/json.h"
#include "json/detail/value.h"
#include <iomanip>

namespace feather::json_detail {
void escape(std::ostream& out,const string& value){out<<'"';for(unsigned char c:value){switch(c){case '"':out<<"\\\"";break;case '\\':out<<"\\\\";break;case '\b':out<<"\\b";break;case '\f':out<<"\\f";break;case '\n':out<<"\\n";break;case '\r':out<<"\\r";break;case '\t':out<<"\\t";break;default:if(c<0x20)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<(int)c<<std::dec;else out<<(char)c;}}out<<'"';}

void writeJson(std::ostream& out,const Json& value,int indent=0) {
    if(std::holds_alternative<std::nullptr_t>(value.value)){out<<"null";return;}
    if(auto boolean=std::get_if<bool>(&value.value)){out<<(*boolean?"true":"false");return;}
    if(auto number=std::get_if<JsonNumber>(&value.value)){out<<number->token;return;}
    if(auto text=std::get_if<string>(&value.value)){escape(out,*text);return;}
    if(auto array=value.array()){out<<'[';for(size_t i=0;i<array->size();++i){if(i)out<<',';out<<'\n'<<string((size_t)indent+2,' ');writeJson(out,(*array)[i],indent+2);}if(!array->empty())out<<'\n'<<string((size_t)indent,' ');out<<']';return;}
    auto object=value.object();out<<'{';size_t index=0;for(const auto& entry:*object){if(index++)out<<',';out<<'\n'<<string((size_t)indent+2,' ');escape(out,entry.first);out<<": ";writeJson(out,entry.second,indent+2);}if(object&&!object->empty())out<<'\n'<<string((size_t)indent,' ');out<<'}';
}
string jsonText(const Json& value){std::ostringstream out;writeJson(out,value);return out.str();}

}

using namespace feather::json_detail;
namespace {
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

bool isValidJson(const string& value) {
    Json parsed;return Parser(value).parse(parsed);
}

wstring jsonValidationMessage(const string& value,bool includeColumn) {
    Json parsed;Parser parser(value);if(parser.parse(parsed))return {};
    size_t position=std::min(parser.errorPosition(),value.size()),line=1,lineStart=0;
    for(size_t i=0;i<position;++i)if(value[i]=='\n'){++line;lineStart=i+1;}
    size_t column=position-lineStart+1;
    if(includeColumn)return L"JSON 格式不正确：第 "+std::to_wstring(line)+L" 行，第 "+std::to_wstring(column)+L" 个字符附近。";
    return L"JSON 格式不正确：第 "+std::to_wstring(line)+L" 行附近。";
}
