#pragma once
#include "release.h"
#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>
namespace uc::update {
struct HttpResponse { unsigned status{}; std::string body; };
using Progress = std::function<void(uint64_t,uint64_t)>;
using DownloadSink = std::function<Status(std::span<const char>)>;
struct Transport {
    virtual ~Transport() = default;
    virtual Result<HttpResponse> get_metadata(std::stop_token stop) = 0;
    virtual Status download(const Release& release,const DownloadSink& sink,std::stop_token stop) = 0;
};
struct WinHttpTransport final:Transport {
    Result<HttpResponse> get_metadata(std::stop_token stop) override;
    Status download(const Release& release,const DownloadSink& sink,std::stop_token stop) override;
};
bool allowed_download_url(std::string_view url,bool redirect);
Result<std::optional<Release>> check_release(Transport& transport,Version installed,std::stop_token stop={});
Status download_release(Transport& transport,const Release& release,const std::filesystem::path& destination,
    Progress progress={},std::stop_token stop={});
}
