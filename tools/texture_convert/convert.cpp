// Upscaled PNGs -> BC7 DDS with a full mip chain, on all cores. File names are kept, so the game finds a replacement by the Dolphin name.
#include "bc7.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace {
struct Image { uint32_t width = 0, height = 0; std::vector<uint8_t> rgba; };

void require(HRESULT hr, const char* what) { if (FAILED(hr)) throw std::runtime_error(std::string(what) + ": HRESULT " + std::to_string(uint32_t(hr))); }

Image read_png(IWICImagingFactory* factory, const fs::path& path) {
    ComPtr<IWICBitmapDecoder> decoder; require(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder), "open PNG");
    ComPtr<IWICBitmapFrameDecode> frame; require(decoder->GetFrame(0, &frame), "PNG frame");
    ComPtr<IWICFormatConverter> convert; require(factory->CreateFormatConverter(&convert), "converter");
    require(convert->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "convert to RGBA");
    Image image; require(convert->GetSize(&image.width, &image.height), "PNG size");
    if (!image.width || !image.height || image.width > 16384 || image.height > 16384) throw std::runtime_error("unsupported PNG size");
    image.rgba.resize(size_t(image.width) * image.height * 4);
    require(convert->CopyPixels(nullptr, image.width * 4, UINT(image.rgba.size()), image.rgba.data()), "PNG pixels");
    return image;
}

// 2x2 box filter, colour weighted by alpha so transparent texels do not darken the edges. Level sizes follow GX: max(1, size / 2).
Image next_level(const Image& source) {
    Image out{std::max(1u, source.width / 2), std::max(1u, source.height / 2), {}};
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (uint32_t y = 0; y < out.height; ++y) for (uint32_t x = 0; x < out.width; ++x) {
        uint32_t color[3] = {}, alpha = 0, plain[3] = {};
        for (uint32_t dy = 0; dy < 2; ++dy) for (uint32_t dx = 0; dx < 2; ++dx) {
            const uint8_t* p = &source.rgba[(size_t(std::min(2 * y + dy, source.height - 1)) * source.width + std::min(2 * x + dx, source.width - 1)) * 4];
            for (int c = 0; c < 3; ++c) { color[c] += p[c] * p[3]; plain[c] += p[c]; }
            alpha += p[3];
        }
        uint8_t* o = &out.rgba[(size_t(y) * out.width + x) * 4];
        for (int c = 0; c < 3; ++c) o[c] = uint8_t(alpha ? (color[c] + alpha / 2) / alpha : (plain[c] + 2) / 4);
        o[3] = uint8_t((alpha + 2) / 4);
    }
    return out;
}

void encode_level(const Image& image, std::vector<uint8_t>& out) {
    const uint32_t bw = (image.width + 3) / 4, bh = (image.height + 3) / 4;
    const size_t start = out.size(); out.resize(start + size_t(bw) * bh * 16);
    uint8_t block[64];
    for (uint32_t by = 0; by < bh; ++by) for (uint32_t bx = 0; bx < bw; ++bx) {
        for (uint32_t i = 0; i < 16; ++i) {
            const uint32_t x = std::min(bx * 4 + i % 4, image.width - 1), y = std::min(by * 4 + i / 4, image.height - 1);
            std::memcpy(block + i * 4, &image.rgba[(size_t(y) * image.width + x) * 4], 4);
        }
        uc::bc7_encode_block(block, &out[start + (size_t(by) * bw + bx) * 16]);
    }
}

void write_dds(const fs::path& path, const Image& base, unsigned mips, const std::vector<uint8_t>& blocks) {
    uint32_t header[37] = {};   // magic + 124-byte header + 20-byte DX10 header
    header[0] = 0x20534444; header[1] = 124;
    header[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000;   // caps, height, width, pixel format, mip count, linear size
    header[3] = base.height; header[4] = base.width;
    header[5] = uint32_t(((base.width + 3) / 4) * size_t((base.height + 3) / 4) * 16);
    header[7] = mips;
    header[19] = 32; header[20] = 4; header[21] = 0x30315844;   // pixel format: FourCC "DX10"
    header[27] = 0x401008;                                      // complex | mipmap | texture
    header[32] = 98; header[33] = 3; header[35] = 1;            // BC7_UNORM, 2D, one layer
    const auto temp = fs::path(path).concat(".tmp");
    { std::ofstream file(temp, std::ios::binary); file.write(reinterpret_cast<const char*>(header), sizeof header); file.write(reinterpret_cast<const char*>(blocks.data()), std::streamsize(blocks.size())); if (!file) throw std::runtime_error("write failed"); }
    fs::rename(temp, path);
}

// Dolphin names with and without the "_m" mip flag share one replacement (the hash covers the base level only).
std::string without_mip_flag(std::string name) {
    if (const auto at = name.find("_m_"); at != std::string::npos && name.find('x') < at) name.erase(at, 2);
    return name;
}

// Extra names the dump gave to one upscaled image (different source blocks, same pixels): scans "file" and "aliases" of base_levels.
std::vector<std::pair<std::string, std::string>> manifest_aliases(const fs::path& manifest) {
    std::ifstream in(manifest, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    std::vector<std::pair<std::string, std::string>> result;
    auto quoted = [&](size_t& at) { const auto open = text.find('"', at), close = text.find('"', open + 1); at = close + 1; return text.substr(open + 1, close - open - 1); };
    size_t at = text.find("\"base_levels\"");
    while (at != std::string::npos && (at = text.find("\"file\"", at)) != std::string::npos) {
        at += 6; const auto file = quoted(at);
        const auto list = text.find("\"aliases\"", at), end = text.find(']', list);
        if (list == std::string::npos || end == std::string::npos) break;
        for (at = text.find('[', list) + 1; text.find('"', at) < end;) result.emplace_back(file, quoted(at));
        at = end;
    }
    return result;
}
std::string dds_name(std::string png) { return png.substr(0, png.size() - 4) + ".dds"; }
} // namespace

int main(int argc, char** argv) {
    fs::path input, output, manifest; unsigned threads = std::max(1u, std::thread::hardware_concurrency()); bool force = false;
    const fs::path exe_parent = [] { wchar_t buf[MAX_PATH]; GetModuleFileNameW(nullptr, buf, MAX_PATH); return fs::path(buf).parent_path().parent_path(); }();
    input = exe_parent / "raw-textures"; output = exe_parent / "textures"; manifest = exe_parent / "TexturesDump" / "RBUE08" / "extractor_manifest.json";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool value = i + 1 < argc;
        if (arg == "--input" && value) input = argv[++i];
        else if (arg == "--output" && value) output = argv[++i];
        else if (arg == "--manifest" && value) manifest = argv[++i];
        else if (arg == "--threads" && value) threads = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--force") force = true;
        else { std::fprintf(stderr, "usage: uc_texture_convert [--input raw-textures] [--output textures] [--manifest extractor_manifest.json] [--threads N] [--force]\n"); return 2; }
    }
    std::error_code error;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(input, error), last; !error && it != last; it.increment(error))
        if (it->path().extension() == ".png") files.push_back(it->path());
    if (error || files.empty()) { std::fprintf(stderr, "no PNG files in %s\n", input.string().c_str()); return 1; }
    fs::create_directories(output);
    // Largest first: the 4096x4096 files then do not end up alone on one thread at the finish.
    std::vector<std::pair<uintmax_t, fs::path>> by_size;
    for (auto& f : files) by_size.emplace_back(fs::file_size(f, error), f);
    std::sort(by_size.rbegin(), by_size.rend());

    std::atomic<size_t> next{0}, done{0}, skipped{0}, failed{0};
    std::mutex log;
    const auto started = std::chrono::steady_clock::now();
    auto work = [&] {
        struct Com { HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED); ~Com() { if (SUCCEEDED(result)) CoUninitialize(); } } com;   // declared first: the factory must go before COM does
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) { failed += by_size.size(); return; }
        for (size_t i; (i = next++) < by_size.size();) {
            const auto& png = by_size[i].second;
            const auto target = output / dds_name(png.filename().string());
            try {
                std::error_code stamp;
                if (!force && fs::exists(target, stamp) && fs::last_write_time(target, stamp) >= fs::last_write_time(png, stamp)) { ++skipped; continue; }
                Image level = read_png(factory.Get(), png);
                const Image base{level.width, level.height, {}};
                std::vector<uint8_t> blocks; unsigned mips = 0;
                for (;;) {
                    encode_level(level, blocks); ++mips;
                    if (level.width == 1 && level.height == 1) break;
                    level = next_level(level);
                }
                write_dds(target, base, mips, blocks);
            } catch (const std::exception& e) {
                ++failed; std::lock_guard lock(log); std::fprintf(stderr, "FAILED %s: %s\n", png.string().c_str(), e.what());
            }
            if (const auto n = ++done; n % 500 == 0) { std::lock_guard lock(log); std::fprintf(stderr, "%zu / %zu\n", n + skipped.load(), by_size.size()); }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(work);
    for (auto& t : pool) t.join();

    size_t linked = 0;
    if (fs::exists(manifest)) {
        for (const auto& [file, alias] : manifest_aliases(manifest)) {
            if (without_mip_flag(alias) == without_mip_flag(file)) continue;
            const auto source = output / dds_name(file), target = output / dds_name(alias);
            if (!fs::exists(source) || fs::exists(target)) continue;
            std::error_code link_error;
            fs::create_hard_link(source, target, link_error);
            if (link_error) fs::copy_file(source, target, link_error);
            if (!link_error) ++linked;
        }
    }
    std::fprintf(stderr, "converted %zu, up to date %zu, failed %zu, alias files %zu, %.1f s\n", done.load(), skipped.load(), failed.load(), linked,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    return failed ? 1 : 0;
}
