#include "download_internal.h"
#include <algorithm>

namespace uc::update {
namespace detail {
Error cancelled() { return {ErrorCode::cancelled,"Update cancelled"}; }
bool ascii_url(std::string_view url) {
    return url.size()<=16384 && url.starts_with("https://") &&
        std::all_of(url.begin(),url.end(),[](unsigned char c){return c>32 && c<127 && c!='\\' && c!='#';});
}
Error status_error(unsigned status) {
    return {(status==403 || status==429)?ErrorCode::rate_limited:ErrorCode::network,
        "GitHub HTTP "+std::to_string(status)};
}
}
bool allowed_download_url(std::string_view url,bool redirect) {
    if(!detail::ascii_url(url)) return false;
    const auto slash=url.find('/',8);
    if(slash==url.npos) return false;
    const auto host=url.substr(8,slash-8);
    if(!redirect) return host=="github.com" &&
        url.substr(slash).starts_with("/solleo95/reuc-recomp/releases/download/");
    return host=="github.com" || host=="release-assets.githubusercontent.com" || host=="objects.githubusercontent.com";
}
Result<std::optional<Release>> check_release(Transport& transport,Version installed,std::stop_token stop,Platform platform) {
    if(stop.stop_requested()) return detail::cancelled();
    auto response=transport.get_metadata(stop);
    if(stop.stop_requested()) return detail::cancelled();
    if(failed(response)) return std::get<Error>(response);
    auto& http=std::get<HttpResponse>(response);
    if(http.status!=200) return detail::status_error(http.status);
    if(http.body.size()>max_metadata_size) return Error{ErrorCode::network,"GitHub response exceeds limit"};   // not GitHub's answer: skipped like a network failure
    return select_release(http.body,installed,platform);
}
}
