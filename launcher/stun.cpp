#include "stun.h"
#include <algorithm>
#include <charconv>

namespace uc::net {
std::optional<Endpoint> parse_peer_address(std::string_view text) {
    const auto begin=text.find_first_not_of(" \t\r\n"),end=text.find_last_not_of(" \t\r\n");
    if(begin==std::string_view::npos)return {};
    text=text.substr(begin,end-begin+1);
    const auto colon=text.find(':');
    if(colon==std::string_view::npos)return {};
    auto ip=std::string(text.substr(0,colon));
    if(!valid_address(ip))return {};
    unsigned first=0;std::from_chars(ip.data(),ip.data()+ip.find('.'),first);
    if(first==0 || first>=224)return {};
    unsigned port=0;
    const auto [next,error]=std::from_chars(text.data()+colon+1,text.data()+text.size(),port);
    if(error!=std::errc{} || next!=text.data()+text.size() || !port || port>65535)return {};
    return Endpoint{false,std::move(ip),uint16_t(port)};
}
std::array<uint8_t,20> stun_request(const std::array<uint8_t,12>& transaction) {
    std::array<uint8_t,20> result{0,1,0,0,0x21,0x12,0xa4,0x42};
    std::copy(transaction.begin(),transaction.end(),result.begin()+8);return result;
}
std::optional<Endpoint> stun_response(std::span<const uint8_t> bytes,const std::array<uint8_t,12>& transaction) {
    const auto request=stun_request(transaction);
    if(bytes.size()<20 || bytes[0]!=1 || bytes[1]!=1 || !std::equal(request.begin()+4,request.end(),bytes.begin()+4))return {};
    const auto u16=[&](size_t i){return unsigned(bytes[i])*256+bytes[i+1];};
    const size_t length=u16(2);
    if(length%4 || length!=bytes.size()-20)return {};
    std::optional<Endpoint> mapped,xored;
    for(size_t at=20;at<bytes.size();) {
        if(bytes.size()-at<4)return {};
        const auto type=u16(at),size=u16(at+2);at+=4;
        const size_t padded=(size+3u)&~3u;
        if(padded>bytes.size()-at)return {};
        if(type==1 || type==0x20) {
            if(size<2 || bytes[at]!=0)return {};
            if(bytes[at+1]==1) {
                if(size!=8)return {};
                const auto port=u16(at+2)^(type==0x20 ? 0x2112u : 0u);
                std::string ip;
                for(size_t i=0;i<4;++i) {
                    if(i)ip+='.';
                    ip+=std::to_string(bytes[at+4+i]^(type==0x20 ? request[4+i] : 0));
                }
                auto endpoint=parse_peer_address(ip+":"+std::to_string(port));
                if(!endpoint)return {};
                (type==0x20 ? xored : mapped)=std::move(endpoint);
            } else if(bytes[at+1]!=2 || size!=20)return {};
        }
        at+=padded;
    }
    return xored ? xored : mapped;
}
}
