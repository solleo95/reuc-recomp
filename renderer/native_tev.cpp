#include "native_tev.h"
#include "pipeline_keys.h"
#define NOMINMAX
#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>

namespace uc {
namespace {
using Microsoft::WRL::ComPtr;
constexpr const char* kFiles[TevShaders::kVariants]{"smoke.spec.hlsl","smoke.motion.spec.hlsl"};
constexpr uint32_t kCacheHeader[2]{0x43544355u,1u};   // "UCTC", version; followed by the 64-bit signature of the sources
constexpr size_t kMaxCacheBytes=256u<<20;
uint64_t fnv(uint64_t hash,const void* data,size_t size) {
    const auto* bytes=static_cast<const unsigned char*>(data);
    for(size_t i=0;i<size;++i) { hash^=bytes[i]; hash*=1099511628211ull; }
    return hash;
}
}
size_t TevShaders::KeyHash::operator()(const Key& key) const {
    return size_t(fnv(fnv(14695981039346656037ull,&key.variant,sizeof key.variant),key.words.data(),sizeof key.words));
}
std::string TevShaders::prelude(const TevWords& words) {
    std::string text="static const uint uc_tev[67]={";
    char number[16];
    for(size_t i=0;i<words.size();++i) { std::snprintf(number,sizeof number,"%s%uu",i ? "," : "",words[i]); text+=number; }
    return text+"};\n#define UC_TEV(i) uc_tev[i]\n";
}
TevShaders::TevShaders(const std::filesystem::path& shader_dir,const std::filesystem::path& cache_dir,Finish finish,bool blocking)
    : finish_(std::move(finish)),blocking_(blocking) {
    uint64_t signature=fnv(14695981039346656037ull,"ps_5_0/O3/1",11);
    for(const char* name:kFiles) {
        std::ifstream file(shader_dir/name,std::ios::binary);
        if(!file) { sources_.clear(); return; }
        sources_.emplace_back(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
        if(sources_.back().empty()) { sources_.clear(); return; }
        signature=fnv(signature,sources_.back().data(),sources_.back().size());
    }
    signature_=signature;
    cache_file_=cache_dir/"dx_tev_cache.bin";
    load_cache();
    for(const auto& [key,code]:cached_) queue_.push_back({key,code,true,{}});   // their shader objects are made ahead of the first draw
    // The keys the Vulkan backend knows are compiled ahead (both variants), behind whatever the running game asks for.
    std::set<TevWords> known;
    for(const char* name:{"pipeline_keys.seed","pipeline_keys.bin"})
        for(const auto& recorded:load_pipeline_keys(cache_dir/name)) {
            if((recorded[0]&3)==2) continue;   // logic operations keep the ubershader
            TevWords words; std::copy(recorded.begin()+1,recorded.begin()+68,words.begin());
            if(known.insert(words).second) for(uint32_t variant=0;variant<kVariants;++variant) if(!cached_.contains({variant,words})) queue_.push_back({{variant,words},{},true,{}});
        }
    const unsigned threads=blocking_ ? 0u : std::clamp(std::thread::hardware_concurrency()/4,2u,4u);
    for(unsigned i=0;i<threads;++i) workers_.emplace_back([this] { run(); });
}
TevShaders::~TevShaders() {
    { std::lock_guard lock(mutex_); stop_=true; queue_.clear(); }
    wake_.notify_all();
    for(auto& worker:workers_) worker.join();
}
void TevShaders::load_cache() {
    std::ifstream file(cache_file_,std::ios::binary);
    uint32_t header[2]; uint64_t signature=0;
    if(!file || !file.read(reinterpret_cast<char*>(header),sizeof header) || !file.read(reinterpret_cast<char*>(&signature),sizeof signature) || !std::equal(header,header+2,kCacheHeader) || signature!=signature_) return;
    cache_ok_=true;
    for(;;) {
        Key key; uint32_t size=0;
        if(!file.read(reinterpret_cast<char*>(&key.variant),sizeof key.variant) || !file.read(reinterpret_cast<char*>(key.words.data()),sizeof key.words) || !file.read(reinterpret_cast<char*>(&size),sizeof size)) return;
        if(key.variant>=kVariants || size==0 || size>(1u<<20)) return;
        auto code=std::make_shared<std::vector<std::byte>>(size);
        if(!file.read(reinterpret_cast<char*>(code->data()),size)) return;   // a torn trailing record is dropped
        cached_.emplace(key,std::move(code));
    }
}
void TevShaders::append_cache(const Key& key,const std::vector<std::byte>& code) {
    std::lock_guard lock(file_mutex_);
    std::error_code error;
    const auto stored=cache_ok_ ? std::filesystem::file_size(cache_file_,error) : 0;
    if(error) cache_ok_=false;   // the file is gone: start it again
    else if(stored>kMaxCacheBytes) return;
    std::ofstream file(cache_file_,cache_ok_ ? std::ios::binary|std::ios::app : std::ios::binary|std::ios::trunc);
    if(!file) return;
    std::string record;   // one write per record, so two game instances cannot interleave halves
    const auto put=[&](const void* data,size_t bytes) { record.append(static_cast<const char*>(data),bytes); };
    if(!cache_ok_) { put(kCacheHeader,sizeof kCacheHeader); put(&signature_,sizeof signature_); cache_ok_=true; }
    const uint32_t size=uint32_t(code.size());
    put(&key.variant,sizeof key.variant); put(key.words.data(),sizeof key.words); put(&size,sizeof size); put(code.data(),code.size());
    file.write(record.data(),std::streamsize(record.size()));
}
TevShaders::Bytecode TevShaders::compile(const Key& key,std::string& error) const {
    const std::string source=prelude(key.words)+sources_[key.variant];
    ComPtr<ID3DBlob> code,messages;
    const HRESULT result=D3DCompile(source.data(),source.size(),kFiles[key.variant],nullptr,nullptr,"fragmentMain","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&messages);
    if(FAILED(result) || !code) {
        error=messages ? std::string(static_cast<const char*>(messages->GetBufferPointer()),messages->GetBufferSize()) : "D3DCompile failed";
        return nullptr;
    }
    const auto* bytes=static_cast<const std::byte*>(code->GetBufferPointer());
    return std::make_shared<const std::vector<std::byte>>(bytes,bytes+code->GetBufferSize());
}
void TevShaders::finish(const Key& key,Bytecode code,bool store) {
    Entry entry{code,{}};
    if(code && finish_) entry.object=finish_(code);
    if(code && !entry.object && finish_) code=entry.code=nullptr;   // the backend refused the bytecode: the ubershader stays
    if(code && store) append_cache(key,*code);
    std::lock_guard lock(mutex_);
    auto& slot=entries_[key];
    slot.entry=std::move(entry); slot.ready=true; ++compiled_;
}
const TevShaders::Entry* TevShaders::get(size_t variant,const TevWords& words) {
    if(sources_.empty() || variant>=kVariants) return nullptr;
    const Key key{uint32_t(variant),words};
    {
        std::unique_lock lock(mutex_);
        const auto found=entries_.find(key);
        if(found!=entries_.end()) return found->second.ready && found->second.entry.code ? &found->second.entry : nullptr;
        entries_.emplace(key,Slot{});
        const auto cached=cached_.find(key);
        Bytecode known=cached!=cached_.end() ? cached->second : nullptr;
        if(!blocking_) { queue_.push_front({key,std::move(known),false,{}}); lock.unlock(); wake_.notify_one(); return nullptr; }
    }
    std::string error;
    auto code=cached_.contains(key) ? cached_.at(key) : compile(key,error);
    if(!code) std::fprintf(stderr,"[pipeline] TEV pixel shader failed to compile: %s\n",error.c_str());
    finish(key,std::move(code),!cached_.contains(key));
    std::lock_guard lock(mutex_);
    const auto& slot=entries_.at(key);
    return slot.entry.code ? &slot.entry : nullptr;
}
void TevShaders::post(std::function<void()> task) {
    if(blocking_) { task(); return; }
    { std::lock_guard lock(mutex_); if(stop_) return; queue_.push_front({{},{},false,std::move(task)}); }
    wake_.notify_one();
}
void TevShaders::run() {
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    for(;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock,[this] { return stop_ || !queue_.empty(); });
            if(stop_) return;
            job=std::move(queue_.front()); queue_.pop_front();
            if(!job.task && job.seed) {   // a pre-compile: skipped when the game already asked for it
                const auto [slot,added]=entries_.try_emplace(job.key);
                (void)slot;
                if(!added) continue;
            }
            ++running_;
        }
        if(job.task) { try { job.task(); } catch(const std::exception& error) { std::fprintf(stderr,"[pipeline] background task failed: %s\n",error.what()); } }
        else {
            bool store=false; Bytecode code=job.code;
            if(!code) {
                std::string error; code=compile(job.key,error); store=code!=nullptr;
                if(!code) std::fprintf(stderr,"[pipeline] TEV pixel shader failed to compile: %s\n",error.c_str());
            }
            finish(job.key,std::move(code),store);
        }
        std::lock_guard lock(mutex_);
        --running_;
    }
}
}
