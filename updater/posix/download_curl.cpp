#include "download_internal.h"
#include "files_posix.h"
#include "sha256.h"
#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

// POSIX twin of updater/download.cpp (WinHTTP): the same policy over libcurl. HTTPS only, every redirect is followed by hand and checked against
// the GitHub allow-list (at most 5), no cookies, no credentials, a deadline per request, and the stop token is polled while the transfer runs.
namespace uc::update {
namespace {
using detail::ascii_url;
using detail::cancelled;
using detail::status_error;
using Clock=std::chrono::steady_clock;
constexpr unsigned max_redirects=5;
Error network_error(std::string action,CURLcode code) {return {ErrorCode::network,std::move(action)+" ("+curl_easy_strerror(code)+")"};}
struct Easy {
    CURL* handle=nullptr;
    Easy():handle(curl_easy_init()) {}
    ~Easy() {if(handle) curl_easy_cleanup(handle);}
};
struct Transfer {
    const DownloadSink* sink=nullptr;
    std::stop_token stop;
    Clock::time_point deadline;
    long status=0;                 // the body is delivered only for 200
    std::string location;
    std::optional<Error> failure;  // an error raised inside a callback
};
size_t on_header(char* data,size_t size,size_t count,void* user) {
    auto& transfer=*static_cast<Transfer*>(user);
    std::string_view line(data,size*count);
    if(line.starts_with("HTTP/")) {   // a status line (one per redirect hop when curl follows; here one per request): the body is delivered only for 200
        const auto space=line.find(' ');
        transfer.status=space==line.npos?0:std::atol(std::string(line.substr(space+1,3)).c_str());
        transfer.location.clear();
    }
    constexpr std::string_view key="location:";
    if(line.size()>key.size() && std::equal(key.begin(),key.end(),line.begin(),[](char a,char b){return a==std::tolower(static_cast<unsigned char>(b));})) {
        auto value=line.substr(key.size());
        while(!value.empty() && (value.front()==' ' || value.front()=='\t')) value.remove_prefix(1);
        while(!value.empty() && (value.back()=='\r' || value.back()=='\n' || value.back()==' ')) value.remove_suffix(1);
        transfer.location.assign(value);
    }
    return size*count;
}
size_t on_body(char* data,size_t size,size_t count,void* user) {
    auto& transfer=*static_cast<Transfer*>(user);
    if(transfer.status!=200 || !transfer.sink) return size*count;   // redirect and error bodies are dropped
    auto result=(*transfer.sink)(std::span<const char>(data,size*count));
    if(failed(result)) {transfer.failure=std::get<Error>(result);return 0;}
    return size*count;
}
// libcurl's sockets must not leak into the game when the launcher is replaced by it (exec_game) or into the restarted launcher.
int on_socket(void*,curl_socket_t fd,curlsocktype) {
    const int flags=::fcntl(fd,F_GETFD);
    if(flags>=0) ::fcntl(fd,F_SETFD,flags|FD_CLOEXEC);
    return CURL_SOCKOPT_OK;
}
int on_progress(void* user,curl_off_t,curl_off_t,curl_off_t,curl_off_t) {
    auto& transfer=*static_cast<Transfer*>(user);
    if(transfer.stop.stop_requested()) {transfer.failure=cancelled();return 1;}
    if(Clock::now()>=transfer.deadline) {transfer.failure=Error{ErrorCode::network,"HTTPS deadline exceeded"};return 1;}
    return 0;
}
// Fetches `url` (following allowed redirects) and returns the final HTTP status; the body of a 200 goes to `sink`.
Result<unsigned> request(std::string url,bool metadata,const DownloadSink& sink,std::stop_token stop) {
    static std::once_flag once;
    std::call_once(once,[]{curl_global_init(CURL_GLOBAL_DEFAULT);});
    if(stop.stop_requested()) return cancelled();
    const auto deadline=Clock::now()+(metadata?std::chrono::seconds(30):std::chrono::seconds(1800));
    for(unsigned redirect=0;redirect<=max_redirects;++redirect) {
        if(stop.stop_requested()) return cancelled();
        if(Clock::now()>=deadline) return Error{ErrorCode::network,"HTTPS deadline exceeded"};
        if(metadata ? (!ascii_url(url) || !url.starts_with("https://api.github.com/")) : !allowed_download_url(url,redirect!=0))
            return Error{ErrorCode::network,"Untrusted HTTPS destination"};
        Easy easy;
        if(!easy.handle) return Error{ErrorCode::network,"Cannot create HTTPS request"};
        Transfer transfer{&sink,stop,deadline,0,{},{}};
        curl_slist* headers=nullptr;
        headers=curl_slist_append(headers,metadata?"Accept: application/vnd.github+json":"Accept: application/octet-stream");
        if(metadata) headers=curl_slist_append(headers,"X-GitHub-Api-Version: 2022-11-28");
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        auto* h=easy.handle;
        CURLcode set=curl_easy_setopt(h,CURLOPT_URL,url.c_str());
        auto chain=[&](CURLcode next){if(set==CURLE_OK) set=next;};
        chain(curl_easy_setopt(h,CURLOPT_PROTOCOLS,static_cast<long>(CURLPROTO_HTTPS)));   // the sysroot's libcurl predates the *_STR options
        chain(curl_easy_setopt(h,CURLOPT_REDIR_PROTOCOLS,static_cast<long>(CURLPROTO_HTTPS)));
        chain(curl_easy_setopt(h,CURLOPT_FOLLOWLOCATION,0L));       // redirects are checked by hand
        chain(curl_easy_setopt(h,CURLOPT_HTTPHEADER,headers));
        chain(curl_easy_setopt(h,CURLOPT_USERAGENT,"UC-Launcher-Update/1"));
        chain(curl_easy_setopt(h,CURLOPT_NOSIGNAL,1L));
        chain(curl_easy_setopt(h,CURLOPT_CONNECTTIMEOUT_MS,5000L));
        chain(curl_easy_setopt(h,CURLOPT_LOW_SPEED_LIMIT,1L));      // abort a hung transfer after 15 s without data
        chain(curl_easy_setopt(h,CURLOPT_LOW_SPEED_TIME,15L));
        chain(curl_easy_setopt(h,CURLOPT_TIMEOUT_MS,static_cast<long>(remaining)));
        chain(curl_easy_setopt(h,CURLOPT_HEADERFUNCTION,on_header));
        chain(curl_easy_setopt(h,CURLOPT_HEADERDATA,&transfer));
        chain(curl_easy_setopt(h,CURLOPT_SOCKOPTFUNCTION,on_socket));
        chain(curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,on_body));
        chain(curl_easy_setopt(h,CURLOPT_WRITEDATA,&transfer));
        chain(curl_easy_setopt(h,CURLOPT_NOPROGRESS,0L));
        chain(curl_easy_setopt(h,CURLOPT_XFERINFOFUNCTION,on_progress));
        chain(curl_easy_setopt(h,CURLOPT_XFERINFODATA,&transfer));
        if(set!=CURLE_OK) {curl_slist_free_all(headers);return network_error("Set HTTPS policy",set);}
        const auto code=curl_easy_perform(h);
        curl_slist_free_all(headers);
        if(transfer.failure) return *transfer.failure;
        if(stop.stop_requested()) return cancelled();
        if(code!=CURLE_OK) return network_error("Request HTTPS",code);
        long status=0;
        curl_easy_getinfo(h,CURLINFO_RESPONSE_CODE,&status);
        if(status==301 || status==302 || status==303 || status==307 || status==308) {
            if(redirect==max_redirects) return Error{ErrorCode::network,"Too many HTTPS redirects"};
            if(transfer.location.empty() || transfer.location.size()>32770 ||
                std::any_of(transfer.location.begin(),transfer.location.end(),[](unsigned char c){return c<33 || c>126;}))
                return Error{ErrorCode::network,"Invalid HTTPS redirect"};
            url=transfer.location;
            continue;
        }
        return static_cast<unsigned>(status);
    }
    return Error{ErrorCode::network,"HTTPS redirect limit"};
}
}
Result<HttpResponse> CurlTransport::get_metadata(std::stop_token stop) {
    std::string body;
    auto status=request("https://api.github.com/repos/solleo95/reuc-recomp/releases/latest",true,
        [&](std::span<const char> bytes)->Status {
            if(bytes.size()>max_metadata_size-body.size()) return Error{ErrorCode::network,"GitHub response exceeds limit"};
            body.append(bytes.data(),bytes.size()); return std::monostate{};
        },stop);
    if(failed(status)) return std::get<Error>(status);
    return HttpResponse{std::get<unsigned>(status),std::move(body)};
}
Status CurlTransport::download(const Release& release,const DownloadSink& sink,std::stop_token stop) {
    auto status=request(release.asset_url,false,sink,stop);
    if(failed(status)) return std::get<Error>(status);
    if(std::get<unsigned>(status)!=200) return status_error(std::get<unsigned>(status));
    return std::monostate{};
}
Status download_release(Transport& transport,const Release& release,const fs::path& destination,Progress progress,std::stop_token stop) {
    if(stop.stop_requested()) return cancelled();
    if(!release.size || release.size>max_zip_size || !allowed_download_url(release.asset_url,false))
        return Error{ErrorCode::invalid_release,"Invalid archive download"};
    struct stat existing{};
    if(::lstat(destination.c_str(),&existing)==0) return Error{ErrorCode::busy,"Download destination already exists"};
    if(errno!=ENOENT) return file_error("Inspect download destination");
    const auto part=fs::path(destination.string()+".part");
    Fd file(::open(part.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
    if(!file.valid()) return file_error("Create partial download");
    uint64_t written=0;
    Sha256 hash;
    auto result=transport.download(release,[&](std::span<const char> bytes)->Status {
        if(stop.stop_requested()) return cancelled();
        if(written>release.size || bytes.size()>release.size-written)
            return Error{ErrorCode::integrity,"Download exceeds release size"};
        size_t offset=0;
        while(offset<bytes.size()) {
            const auto wrote=::write(file.get(),bytes.data()+offset,bytes.size()-offset);
            if(wrote<0&&errno==EINTR) continue;
            if(wrote<=0) return file_error("Write download");
            offset+=static_cast<size_t>(wrote);
        }
        hash.update({reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size()});
        written+=bytes.size();
        if(progress) progress(written,release.size);
        return std::monostate{};
    },stop);
    if(stop.stop_requested()) result=cancelled();
    if(!failed(result)) {
        if(written!=release.size) result=Error{ErrorCode::integrity,"Incomplete download"};
        else if(::fsync(file.get())!=0) result=file_error("Flush download");
        else if(hash.finish_hex()!=release.sha256) result=Error{ErrorCode::integrity,"Download SHA-256 mismatch"};
    }
    file=Fd{};
    if(!failed(result)) result=replace_file(part,destination);
    if(failed(result) && ::unlink(part.c_str())!=0 && errno!=ENOENT)
        std::get<Error>(result).detail+="; partial download retained";
    return result;
}
}
