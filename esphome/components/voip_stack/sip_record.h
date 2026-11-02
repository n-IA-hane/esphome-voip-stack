#pragma once
#include "sip_header_name.h"
#include <string>
#include <string_view>
namespace esphome::voip_stack {
inline std::string_view sip_record_trim(std::string_view value) {
 const auto first=value.find_first_not_of(" \t");
 return first==std::string_view::npos ? std::string_view{} : value.substr(first,value.find_last_not_of(" \t")-first+1);
}
template<class F> void sip_record_fields(std::string_view message,std::string_view wanted,F consume) {
 size_t at=message.find("\r\n");
 while(at!=std::string_view::npos){
  const size_t start=at+2,end=message.find("\r\n",start);
  if(end==std::string_view::npos||end==start)break;
  size_t last=end,next=message.find("\r\n",end+2);
  while(end+2<message.size()&&last+2<message.size()&&(message[last+2]==' '||message[last+2]=='\t')&&next!=std::string_view::npos){last=next;next=message.find("\r\n",last+2);}
  const auto colon=message.find(':',start);
  if(colon<end&&sip_header_name_equal(sip_record_trim(message.substr(start,colon-start)),wanted))
   if(!consume(sip_record_trim(message.substr(colon+1,last-colon-1))))return;
  at=last;
 }
}
inline std::string_view sip_record_first(std::string_view message,std::string_view name) {
 std::string_view value;sip_record_fields(message,name,[&](std::string_view field){value=field;return false;});return value;
}
inline std::string sip_record_unfold(std::string_view field) {
 std::string result;
 for(size_t start=0;start<field.size();){
  const auto end=field.find("\r\n",start);const auto part=sip_record_trim(field.substr(start,end-start));
  if (!result.empty()) result += ' ';
  result.append(part.data(), part.size());
  if (end == std::string_view::npos) break;
  start = end + 2;
 }
 return result;
}
}
