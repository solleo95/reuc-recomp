#pragma once
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace uc {
// A draw's TEV setup: stage count, swap tables, 16 x {color, alpha, order, konst}, destination alpha pass (the pipeline key words 1..67 of the Vulkan backend).
using TevWords=std::array<uint32_t,67>;

// Specialized TEV pixel shaders for the DirectX backends. Vulkan folds a draw's TEV words into specialization constants; DXBC has none, so the ubershader's
// HLSL (smoke.spec.hlsl, smoke.motion.spec.hlsl: shaders/smoke.slang with -DUBER -DUC_SPEC) is compiled again with the words as a constant table in front.
// The result is a few KB of straight-line code instead of the ubershader's 340 KB (several times the GPU cost). Compiles run on background threads
// (~0.1 s each); until one is ready the draw keeps the ubershader. Finished bytecode is stored in data/dx_tev_cache.bin and the keys the Vulkan
// backend recorded (pipeline_keys.seed / .bin) are compiled ahead, so a second launch finds everything ready.
class TevShaders {
public:
    static constexpr size_t kVariants=2;   // 0: scene-less packets, 1: scene packets (motion, G-buffer and RT outputs)
    using Bytecode=std::shared_ptr<const std::vector<std::byte>>;
    struct Entry { Bytecode code; std::shared_ptr<void> object; };   // object: what Finish made from the code on the worker (D3D11 shader object)
    using Finish=std::function<std::shared_ptr<void>(const Bytecode&)>;
    // shader_dir holds the .hlsl files, cache_dir the cache and key files. blocking: compile inside get() (tests). Unavailable (no files or compiler): get() returns null.
    TevShaders(const std::filesystem::path& shader_dir,const std::filesystem::path& cache_dir,Finish finish={},bool blocking=false);
    ~TevShaders();
    bool available() const { return !sources_.empty(); }
    bool blocking() const { return blocking_; }
    // The ready entry, or null while the shader compiles (the first call queues it, ahead of the pre-compiles) or when it cannot be compiled.
    const Entry* get(size_t variant,const TevWords& words);
    void post(std::function<void()> task);   // runs on a worker (before the pre-compiles); the renderers build their pipelines with it
    size_t compiled() const { std::lock_guard lock(mutex_); return compiled_; }
    size_t pending() const { std::lock_guard lock(mutex_); return queue_.size()+running_; }
    static std::string prelude(const TevWords& words);   // the constant table and UC_TEV macro put in front of the HLSL
private:
    struct Key {
        uint32_t variant; TevWords words;
        bool operator==(const Key&) const=default;
    };
    struct KeyHash { size_t operator()(const Key& key) const; };
    struct Slot { Entry entry; bool ready=false; };
    struct Job { Key key{}; Bytecode code; bool seed=false; std::function<void()> task; };   // code: loaded from the cache file, only Finish is left
    Bytecode compile(const Key& key,std::string& error) const;
    void finish(const Key& key,Bytecode code,bool store);
    void run();
    void load_cache();
    void append_cache(const Key& key,const std::vector<std::byte>& code);
    std::vector<std::string> sources_;
    std::filesystem::path cache_file_;
    uint64_t signature_=0;
    Finish finish_;
    bool blocking_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::unordered_map<Key,Slot,KeyHash> entries_;
    std::unordered_map<Key,Bytecode,KeyHash> cached_;   // read from the cache file at construction, never changed after
    std::deque<Job> queue_;
    std::vector<std::thread> workers_;
    std::mutex file_mutex_;
    size_t compiled_=0,running_=0,demanded_=0;   // demanded_: shaders the game asked for before they were ready (each drew with the ubershader meanwhile)
    double compile_ms_=0;   // worker time spent compiling since the last idle report
    bool stop_=false,cache_ok_=false;
};
}
