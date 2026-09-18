#pragma once

#include "domain/normalization.h"
#include "platform/encoding.h"
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <variant>
#include <vector>

namespace feather::json_detail {
using std::string;
using std::wstring;

struct JsonNumber { string token; };

struct Json {
    using Object = std::vector<std::pair<string, Json>>;
    using Array = std::vector<Json>;
    std::variant<std::nullptr_t, bool, JsonNumber, string, Object, Array> value;
    Json() : value(nullptr) {}
    Json(bool v) : value(v) {}
    Json(JsonNumber v) : value(std::move(v)) {}
    Json(string v) : value(std::move(v)) {}
    Json(Object v) : value(std::move(v)) {}
    Json(Array v) : value(std::move(v)) {}
    const Object* object() const { return std::get_if<Object>(&value); }
    const Array* array() const { return std::get_if<Array>(&value); }
    string text(const string& fallback = {}) const { auto p=std::get_if<string>(&value); return p?*p:fallback; }
    bool boolean(bool fallback=true) const { auto p=std::get_if<bool>(&value); return p?*p:fallback; }
    int integer(int fallback) const {
        auto p=std::get_if<JsonNumber>(&value);if(!p)return fallback;
        std::istringstream input(p->token);input.imbue(std::locale::classic());double parsed=0;
        if(!(input>>parsed)||!std::isfinite(parsed)||parsed<std::numeric_limits<int>::min()||parsed>std::numeric_limits<int>::max())return fallback;
        return (int)parsed;
    }
    const Json* get(const char* key) const { auto o=object();if(!o)return nullptr;for(const auto& entry:*o)if(entry.first==key)return &entry.second;return nullptr; }
    const Json* getInsensitive(const char* key) const { auto o=object();if(!o)return nullptr;for(const auto& entry:*o)if(equalsIgnoreCase(entry.first,key))return &entry.second;return nullptr; }
};

class Parser {
public:
    explicit Parser(const string& source) : source_(source) {}
    bool parse(Json& output) { skip(); if(!value(output))return false; skip(); return position_==source_.size(); }
    size_t errorPosition() const { return position_; }
private:
    const string& source_; size_t position_=0;
    void skip(){while(position_<source_.size()&&(source_[position_]==' '||source_[position_]=='\t'||source_[position_]=='\r'||source_[position_]=='\n'))++position_;}
    bool take(char c){skip();if(position_<source_.size()&&source_[position_]==c){++position_;return true;}return false;}
    bool value(Json& out){
        skip(); if(position_>=source_.size())return false; char c=source_[position_];
        if(c=='{')return object(out); if(c=='[')return array(out); if(c=='"'){string s;if(!quoted(s))return false;out=Json(std::move(s));return true;}
        if(c=='t'&&source_.compare(position_,4,"true")==0){position_+=4;out=Json(true);return true;}
        if(c=='f'&&source_.compare(position_,5,"false")==0){position_+=5;out=Json(false);return true;}
        if(c=='n'&&source_.compare(position_,4,"null")==0){position_+=4;out=Json();return true;}
        return number(out);
    }
    bool object(Json& out){
        if(!take('{'))return false; Json::Object result; skip(); if(take('}')){out=Json(std::move(result));return true;}
        for(;;){string key;if(!quoted(key)||!take(':'))return false;Json item;if(!value(item))return false;auto existing=std::find_if(result.begin(),result.end(),[&](const auto& entry){return entry.first==key;});if(existing==result.end())result.emplace_back(std::move(key),std::move(item));else existing->second=std::move(item);if(take('}'))break;if(!take(','))return false;}
        out=Json(std::move(result));return true;
    }
    bool array(Json& out){
        if(!take('['))return false; Json::Array result; skip();if(take(']')){out=Json(std::move(result));return true;}
        for(;;){Json item;if(!value(item))return false;result.push_back(std::move(item));if(take(']'))break;if(!take(','))return false;}
        out=Json(std::move(result));return true;
    }
    bool quoted(string& out){
        skip();if(position_>=source_.size()||source_[position_++]!='"')return false;out.clear();
        while(position_<source_.size()){
            unsigned char c=(unsigned char)source_[position_++];if(c=='"')return true;if(c<0x20)return false;if(c!='\\'){out+=(char)c;continue;}
            if(position_>=source_.size())return false;char e=source_[position_++];
            switch(e){case '"':out+='"';break;case '\\':out+='\\';break;case '/':out+='/';break;case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
            case 'u':{
                unsigned code=0;if(!hexCodeUnit(code))return false;
                if(code>=0xD800&&code<=0xDBFF){
                    if(position_+2>source_.size()||source_[position_]!='\\'||source_[position_+1]!='u')return false;position_+=2;
                    unsigned low=0;if(!hexCodeUnit(low)||low<0xDC00||low>0xDFFF)return false;wstring pair;pair+=(wchar_t)code;pair+=(wchar_t)low;out+=toUtf8(pair);
                }else if(code>=0xDC00&&code<=0xDFFF)return false;
                else out+=toUtf8(wstring(1,(wchar_t)code));break;
            }default:return false;}
        }return false;
    }
    bool hexCodeUnit(unsigned& code){
        if(position_+4>source_.size())return false;code=0;for(int i=0;i<4;++i){char h=source_[position_++];unsigned digit=0;if(h>='0'&&h<='9')digit=(unsigned)(h-'0');else if(h>='a'&&h<='f')digit=(unsigned)(h-'a'+10);else if(h>='A'&&h<='F')digit=(unsigned)(h-'A'+10);else return false;code=(code<<4)+digit;}return true;
    }
    bool number(Json& out){
        size_t start=position_;if(position_<source_.size()&&source_[position_]=='-')++position_;
        if(position_>=source_.size())return false;
        if(source_[position_]=='0')++position_;else if(source_[position_]>='1'&&source_[position_]<='9'){while(position_<source_.size()&&std::isdigit((unsigned char)source_[position_]))++position_;}else return false;
        if(position_<source_.size()&&source_[position_]=='.'){++position_;size_t fraction=position_;while(position_<source_.size()&&std::isdigit((unsigned char)source_[position_]))++position_;if(position_==fraction)return false;}
        if(position_<source_.size()&&(source_[position_]=='e'||source_[position_]=='E')){++position_;if(position_<source_.size()&&(source_[position_]=='+'||source_[position_]=='-'))++position_;size_t exponent=position_;while(position_<source_.size()&&std::isdigit((unsigned char)source_[position_]))++position_;if(position_==exponent)return false;}
        out=Json(JsonNumber{source_.substr(start,position_-start)});return true;
    }
};


void escape(std::ostream& out, const string& value);
string jsonText(const Json& value);
}
