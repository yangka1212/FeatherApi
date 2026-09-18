#pragma once

#include <algorithm>
#include <cctype>
#include <string>

inline std::string trimText(std::string value) { while(!value.empty()&&std::isspace((unsigned char)value.front()))value.erase(value.begin());while(!value.empty()&&std::isspace((unsigned char)value.back()))value.pop_back();return value; }
inline bool equalsIgnoreCase(const std::string& left,const std::string& right){return left.size()==right.size()&&std::equal(left.begin(),left.end(),right.begin(),[](unsigned char a,unsigned char b){return std::tolower(a)==std::tolower(b);});}
inline bool containsIgnoreCase(const std::string& value,const std::string& search){if(search.empty())return true;auto lower=[](unsigned char c){return (char)std::tolower(c);};return std::search(value.begin(),value.end(),search.begin(),search.end(),[&](char left,char right){return lower((unsigned char)left)==lower((unsigned char)right);})!=value.end();}
inline std::string normalizedName(std::string value,const std::string& fallback){value=trimText(value);return value.empty()?fallback:value;}
inline std::string normalizedMethod(std::string value){value=trimText(value);if(value.empty())return "GET";for(char& c:value)c=(char)std::toupper((unsigned char)c);return value;}
inline std::string normalizedParameterType(std::string value){value=trimText(value);if(equalsIgnoreCase(value,"file")||containsIgnoreCase(value,"binary"))return "file";if(equalsIgnoreCase(value,"number")||equalsIgnoreCase(value,"int")||equalsIgnoreCase(value,"integer")||containsIgnoreCase(value,"int32")||containsIgnoreCase(value,"int64")||equalsIgnoreCase(value,"float")||equalsIgnoreCase(value,"double")||equalsIgnoreCase(value,"decimal"))return "number";return "string";}
