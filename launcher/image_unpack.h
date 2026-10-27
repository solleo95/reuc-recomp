#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
namespace uc::launcher {
constexpr uint64_t unpacked_bytes=4ull<<30;   // shortcut: the NTSC-U disc unpacks to 3.8 GiB; other sizes are not needed because other discs are refused
enum class UnpackStatus {running,done,failed,cancelled};
enum class UnpackError {none,wit_missing,wrong_image,image_in_game_folder,no_space,extract_failed};
// The last "NN%" of Wit's progress output ("  17% copied in 0:04 (175.1 MiB/sec)"), if any.
std::optional<int> parse_percent(std::string_view output);
// Unpacks a disc image into <root>\game with Wit on a worker thread. Whatever is in <root>\game is deleted first, but only after the image was
// identified as RBUE08 (NTSC-U); a failed or cancelled unpack leaves no game folder behind. The destructor cancels and waits.
class ImageUnpack {
public:
    // `wit` defaults to <root>\tools\wit\wit.exe; `required_bytes` is the free space needed (the old game folder counts as free).
    ImageUnpack(std::filesystem::path root,std::filesystem::path image,std::filesystem::path wit={},uint64_t required_bytes=unpacked_bytes);
    ~ImageUnpack();
    ImageUnpack(const ImageUnpack&)=delete;
    ImageUnpack& operator=(const ImageUnpack&)=delete;
    void cancel();
    UnpackStatus status() const {return status_;}
    UnpackError error() const {return error_;}
    int percent() const {return percent_;}
private:
    void run();
    bool stopped();
    // Runs Wit with a UTF-8 locale (without it Wit cannot open non-ASCII paths) and returns its exit code; `output` gets everything it prints.
    int wit(const std::wstring& arguments,std::string& output,bool track_progress);
    std::filesystem::path root_,image_,wit_;
    uint64_t required_bytes_;
    std::atomic<UnpackStatus> status_{UnpackStatus::running};
    std::atomic<UnpackError> error_{UnpackError::none};
    std::atomic<int> percent_{0};
    std::mutex process_lock_;
    void* process_=nullptr;
    bool cancelled_=false;
    std::jthread worker_;
};
}
