#include "download.h"
#include "files.h"
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>

namespace uc::update {
namespace {
Error network_error(std::string action) {
    return {ErrorCode::network,std::move(action)+" (Windows "+std::to_string(GetLastError())+")"};
}
Error cancelled() { return {ErrorCode::cancelled,"Update cancelled"}; }
struct HttpCloser { void operator()(void* handle) const { if(handle) WinHttpCloseHandle(handle); } };
using HttpHandle=std::unique_ptr<void,HttpCloser>;
bool ascii_url(std::string_view url) {
    return url.size()<=16384 && url.starts_with("https://") &&
        std::all_of(url.begin(),url.end(),[](unsigned char c){return c>32 && c<127 && c!='\\' && c!='#';});
}
Result<unsigned> request(std::string url,bool metadata,const DownloadSink& sink,std::stop_token stop) {
    if(stop.stop_requested()) return cancelled();
    HttpHandle session(WinHttpOpen(L"UC-Launcher-Update/1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
    if(!session) return network_error("Open HTTPS session");
    if(!WinHttpSetTimeouts(session.get(),5000,5000,15000,15000)) return network_error("Set HTTPS timeouts");
    const auto deadline=std::chrono::steady_clock::now()+(metadata?std::chrono::seconds(30):std::chrono::seconds(1800));
    for(unsigned redirect=0;redirect<=5;++redirect) {
        if(stop.stop_requested()) return cancelled();
        if(std::chrono::steady_clock::now()>=deadline) return Error{ErrorCode::network,"HTTPS deadline exceeded"};
        if(metadata ? (!ascii_url(url) || !url.starts_with("https://api.github.com/")) :
            !allowed_download_url(url,redirect!=0))
            return Error{ErrorCode::network,"Untrusted HTTPS destination"};
        std::wstring wide(url.begin(),url.end());
        URL_COMPONENTS parts{}; parts.dwStructSize=sizeof(parts);
        parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=static_cast<DWORD>(-1);
        if(!WinHttpCrackUrl(wide.c_str(),static_cast<DWORD>(wide.size()),0,&parts))
            return network_error("Parse HTTPS destination");
        std::wstring host(parts.lpszHostName,parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath,parts.dwUrlPathLength);
        if(parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
        HttpHandle connection(WinHttpConnect(session.get(),host.c_str(),INTERNET_DEFAULT_HTTPS_PORT,0));
        if(!connection) return network_error("Connect HTTPS");
        HttpHandle req(WinHttpOpenRequest(connection.get(),L"GET",path.c_str(),nullptr,
            WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
        if(!req) return network_error("Create HTTPS request");
        DWORD policy=WINHTTP_OPTION_REDIRECT_POLICY_NEVER, auth=WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        DWORD disabled=WINHTTP_DISABLE_COOKIES;
        if(!WinHttpSetOption(req.get(),WINHTTP_OPTION_REDIRECT_POLICY,&policy,sizeof(policy)) ||
            !WinHttpSetOption(req.get(),WINHTTP_OPTION_AUTOLOGON_POLICY,&auth,sizeof(auth)) ||
            !WinHttpSetOption(req.get(),WINHTTP_OPTION_DISABLE_FEATURE,&disabled,sizeof(disabled)))
            return network_error("Set HTTPS policy");
        const wchar_t* headers=metadata?L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n":
            L"Accept: application/octet-stream\r\n";
        if(!WinHttpSendRequest(req.get(),headers,static_cast<DWORD>(-1),nullptr,0,0,0) ||
            !WinHttpReceiveResponse(req.get(),nullptr)) return network_error("Request HTTPS");
        DWORD status=0, status_size=sizeof(status);
        if(!WinHttpQueryHeaders(req.get(),WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,&status,&status_size,WINHTTP_NO_HEADER_INDEX))
            return network_error("Read HTTP status");
        if(status==301 || status==302 || status==303 || status==307 || status==308) {
            if(redirect==5) return Error{ErrorCode::network,"Too many HTTPS redirects"};
            DWORD bytes=0;
            WinHttpQueryHeaders(req.get(),WINHTTP_QUERY_LOCATION,WINHTTP_HEADER_NAME_BY_INDEX,nullptr,&bytes,WINHTTP_NO_HEADER_INDEX);
            if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER || bytes>32770 || bytes<sizeof(wchar_t))
                return Error{ErrorCode::network,"Invalid HTTPS redirect"};
            std::vector<wchar_t> location(bytes/sizeof(wchar_t)+1,L'\0');
            if(!WinHttpQueryHeaders(req.get(),WINHTTP_QUERY_LOCATION,WINHTTP_HEADER_NAME_BY_INDEX,
                location.data(),&bytes,WINHTTP_NO_HEADER_INDEX)) return network_error("Read HTTPS redirect");
            std::wstring_view value(location.data());
            if(std::any_of(value.begin(),value.end(),[](wchar_t c){return c<33 || c>126;}))
                return Error{ErrorCode::network,"Invalid HTTPS redirect characters"};
            url.assign(value.begin(),value.end());
            continue;
        }
        if(status!=200) return status;
        std::array<char,65536> buffer{};
        for(;;) {
            if(stop.stop_requested()) return cancelled();
            if(std::chrono::steady_clock::now()>=deadline) return Error{ErrorCode::network,"HTTPS deadline exceeded"};
            DWORD got=0;
            if(!WinHttpReadData(req.get(),buffer.data(),static_cast<DWORD>(buffer.size()),&got))
                return network_error("Read HTTPS response");
            if(!got) break;
            auto result=sink(std::span<const char>(buffer.data(),got));
            if(failed(result)) return std::get<Error>(result);
        }
        if(stop.stop_requested()) return cancelled();
        return status;
    }
    return Error{ErrorCode::network,"HTTPS redirect limit"};
}
Error status_error(unsigned status) {
    return {(status==403 || status==429)?ErrorCode::rate_limited:ErrorCode::network,
        "GitHub HTTP "+std::to_string(status)};
}
}
bool allowed_download_url(std::string_view url,bool redirect) {
    if(!ascii_url(url)) return false;
    const auto slash=url.find('/',8);
    if(slash==url.npos) return false;
    const auto host=url.substr(8,slash-8);
    if(!redirect) return host=="github.com" &&
        url.substr(slash).starts_with("/solleo95/reuc-recomp/releases/download/");
    return host=="github.com" || host=="release-assets.githubusercontent.com" || host=="objects.githubusercontent.com";
}
Result<HttpResponse> WinHttpTransport::get_metadata(std::stop_token stop) {
    std::string body;
    auto status=request("https://api.github.com/repos/solleo95/reuc-recomp/releases/latest",true,
        [&](std::span<const char> bytes)->Status {
            if(bytes.size()>max_metadata_size-body.size()) return Error{ErrorCode::invalid_release,"GitHub response exceeds limit"};
            body.append(bytes.data(),bytes.size()); return std::monostate{};
        },stop);
    if(failed(status)) return std::get<Error>(status);
    return HttpResponse{std::get<unsigned>(status),std::move(body)};
}
Status WinHttpTransport::download(const Release& release,const DownloadSink& sink,std::stop_token stop) {
    auto status=request(release.asset_url,false,sink,stop);
    if(failed(status)) return std::get<Error>(status);
    if(std::get<unsigned>(status)!=200) return status_error(std::get<unsigned>(status));
    return std::monostate{};
}
Result<std::optional<Release>> check_release(Transport& transport,Version installed,std::stop_token stop) {
    if(stop.stop_requested()) return cancelled();
    auto response=transport.get_metadata(stop);
    if(stop.stop_requested()) return cancelled();
    if(failed(response)) return std::get<Error>(response);
    auto& http=std::get<HttpResponse>(response);
    if(http.status!=200) return status_error(http.status);
    return select_release(http.body,installed);
}
Status download_release(Transport& transport,const Release& release,const fs::path& destination,
    Progress progress,std::stop_token stop) {
    if(stop.stop_requested()) return cancelled();
    if(!release.size || release.size>max_zip_size || !allowed_download_url(release.asset_url,false))
        return Error{ErrorCode::invalid_release,"Invalid archive download"};
    auto name=destination.filename().u8string();
    auto parent=lock_parent(destination.parent_path(),std::string(name.begin(),name.end()));
    if(failed(parent)) return std::get<Error>(parent);
    if(GetFileAttributesW(destination.c_str())!=INVALID_FILE_ATTRIBUTES)
        return Error{ErrorCode::busy,"Download destination already exists"};
    auto part=fs::path(destination.wstring()+L".part");
    Handle file(CreateFileW(part.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!file.valid()) return file_error("Create partial download");
    uint64_t written=0;
    auto result=transport.download(release,[&](std::span<const char> bytes)->Status {
        if(stop.stop_requested()) return cancelled();
        if(written>release.size || bytes.size()>release.size-written)
            return Error{ErrorCode::integrity,"Download exceeds release size"};
        size_t offset=0;
        while(offset<bytes.size()) {
            DWORD count=0, wanted=static_cast<DWORD>(std::min<size_t>(bytes.size()-offset,65536));
            if(!WriteFile(file.get(),bytes.data()+offset,wanted,&count,nullptr) || !count) return file_error("Write download");
            offset+=count;
        }
        written+=bytes.size();
        if(progress) progress(written,release.size);
        return std::monostate{};
    },stop);
    if(stop.stop_requested()) result=cancelled();
    if(!failed(result)) {
        if(written!=release.size) result=Error{ErrorCode::integrity,"Incomplete download"};
        else if(!FlushFileBuffers(file.get())) result=file_error("Flush download");
        else {
            auto hash=sha256_handle(file.get());
            if(failed(hash)) result=std::get<Error>(hash);
            else if(std::get<std::string>(hash)!=release.sha256) result=Error{ErrorCode::integrity,"Download SHA-256 mismatch"};
        }
    }
    if(!failed(result)) result=rename_sibling(file.get(),destination.filename().wstring());
    file=Handle{};
    if(failed(result) && !DeleteFileW(part.c_str()) && GetLastError()!=ERROR_FILE_NOT_FOUND)
        std::get<Error>(result).detail+="; partial download retained";
    return result;
}
}
