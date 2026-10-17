#include "cpu_calibration.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <nlohmann/json.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
namespace uc {
namespace {
using Clock=std::chrono::steady_clock;
using Json=nlohmann::json;
constexpr int profile_version=2;
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
bool valid_cpu(int cpu,const CpuTopology& topology) {
    return cpu>=0 && cpu<63 && std::ranges::any_of(topology.cores,[&](const CpuCore& core){return core.cpu==cpu;});
}
// shortcut: this bounded memory/transform probe approximates guest CPU pressure;
// replace it with a captured guest kernel if fight3 does not validate its ranking.
struct alignas(64) Node {uint32_t next;float x,y,z;float padding[12];};
struct Probe {
    std::vector<Node> nodes=std::vector<Node>(1<<18);
    uint32_t index=0;
    double x=.1,y=.2,z=.3;
    Probe() {
        std::vector<uint32_t> order(nodes.size());std::iota(order.begin(),order.end(),0);
        uint32_t random=12345;
        for(size_t i=order.size()-1;i>0;--i) {random=random*1664525u+1013904223u;std::swap(order[i],order[random%(i+1)]);}
        for(size_t i=0;i<order.size();++i)nodes[order[i]]={order[(i+1)%order.size()],float(i%17)*.01f,float(i%13)*.01f,float(i%7)*.01f,{}};
    }
    __declspec(noinline) void batch() {
        for(int i=0;i<4096;++i) {
            const auto& node=nodes[index];index=node.next;
            const double a=x*.31+y*.17+z*.11+node.x;
            const double b=x*.13+y*.29+z*.19+node.y;
            z=x*.23+y*.07+z*.37+node.z;x=a;y=b;
        }
    }
    double sample(double milliseconds,std::stop_token stop) {
        index=0;x=.1;y=.2;z=.3;
        const auto start=Clock::now();uint64_t batches=0;double elapsed=0;
        do {batch();++batches;elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
        while(elapsed<milliseconds && !stop.stop_requested());
        if(!std::isfinite(x+y+z))throw std::runtime_error("CPU probe arithmetic failed");
        return double(batches)*4096/elapsed;
    }
};
void save_profile(const std::filesystem::path& path,const CpuTopology& topology,int cpu,const std::vector<CpuScore>& scores) {
    Json data={{"version",profile_version},{"machine",topology.fingerprint},{"cpu",cpu}};
    for(const auto& score:scores)data["scores"].push_back({{"cpu",score.cpu},{"samples",score.samples},{"scheduling_class",score.scheduling_class}});
    const auto bytes=data.dump(2)+"\n";
    auto temporary=path;temporary+=L".tmp-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    bool created=false;
    try {
        {
            Handle file{CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
            if(file.value==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot create CPU profile");
            created=true;DWORD written=0;
            if(!WriteFile(file.value,bytes.data(),DWORD(bytes.size()),&written,nullptr) || written!=bytes.size() || !FlushFileBuffers(file.value))
                throw std::runtime_error("cannot write CPU profile");
        }
        if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("cannot replace CPU profile");
    } catch(...) {
        if(created && !DeleteFileW(temporary.c_str()))std::fprintf(stderr,"[cpu-calibration] temporary profile cleanup failed: %lu\n",GetLastError());
        throw;
    }
}
CpuCalibrationResult calibrate(const std::filesystem::path& path,std::stop_token stop) {
    CpuCalibrationResult result;
    const auto topology=cpu_topology();
    if(topology.cores.empty()) {result.error="CPU topology unsupported; using Windows scheduling";return result;}
    if(auto cached=load_cpu_profile(path,topology)) {result.cpu=*cached;result.cached=true;return result;}
    if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
    auto lock_path=path;lock_path+=L".calibrating";
    Handle lock;
    const auto deadline=Clock::now()+std::chrono::seconds(12);
    while(!stop.stop_requested()) {
        lock.value=CreateFileW(lock_path.c_str(),GENERIC_WRITE|DELETE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_HIDDEN|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        if(lock.value!=INVALID_HANDLE_VALUE)break;
        const auto error=GetLastError();
        if(error!=ERROR_SHARING_VIOLATION)throw std::runtime_error("CPU calibration lock unavailable");
        if(Clock::now()>=deadline)throw std::runtime_error("CPU calibration busy or profile unwritable");
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if(stop.stop_requested())return result;
    if(auto cached=load_cpu_profile(path,topology)) {result.cpu=*cached;result.cached=true;return result;}
    Probe probe;
    std::vector<CpuScore> scores;
    for(const auto& core:topology.cores)scores.push_back({core.cpu,{},core.scheduling_class});
    // Alternate traversal so late cores do not systematically get the warm run.
    const double sample_ms=std::clamp(1920.0/scores.size()/3,15.0,60.0);
    DWORD_PTR previous=0;
    struct RestoreAffinity {DWORD_PTR& mask;~RestoreAffinity(){if(mask && !SetThreadAffinityMask(GetCurrentThread(),mask))std::fprintf(stderr,"[cpu-calibration] affinity restore failed\n");}} restore{previous};
    for(size_t round=0;round<3;++round)for(size_t n=0;n<scores.size();++n) {
        if(stop.stop_requested())return result;
        const size_t i=round==1 ? scores.size()-1-n : (n+round*(scores.size()/2))%scores.size();
        const auto old=SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR(1)<<scores[i].cpu);
        if(!old)throw std::runtime_error("cannot select CPU for calibration");
        if(!previous)previous=old;
        probe.sample(8,stop);
        scores[i].samples[round]=probe.sample(sample_ms,stop);
    }
    if(stop.stop_requested())return result;
    result.cpu=fastest_cpu(scores);
    for(const auto& score:scores)std::fprintf(stderr,"[cpu-calibration] CPU %d class %u samples %.1f %.1f %.1f\n",score.cpu,score.scheduling_class,score.samples[0],score.samples[1],score.samples[2]);
    if(result.cpu>=0) {
        try {save_profile(path,topology,result.cpu,scores);}
        catch(const std::exception& error) {result.error=error.what();}
    }
    return result;
}
}
int fastest_cpu(const std::vector<CpuScore>& scores) {
    struct Candidate {int cpu;double rate;unsigned scheduling_class;};
    std::vector<Candidate> candidates;double fastest=0;
    for(const auto& score:scores) {
        if(score.cpu<0 || score.cpu>=63 || std::ranges::any_of(score.samples,[](double sample){return !std::isfinite(sample) || sample<=0;}))continue;
        auto sorted=score.samples;std::sort(sorted.begin(),sorted.end());
        candidates.push_back({score.cpu,sorted[1],score.scheduling_class});
        fastest=std::max(fastest,sorted[1]);
    }
    // A 5% tie band is a conservative heuristic, not a confidence interval.
    // Scheduling hints may break probe ties, never override clearly faster work.
    Candidate best{-1,0,0};
    for(const auto& candidate:candidates)if(candidate.rate>=fastest*.95 &&
        (best.cpu<0 || candidate.scheduling_class>best.scheduling_class ||
         (candidate.scheduling_class==best.scheduling_class && candidate.rate>best.rate)))best=candidate;
    return best.cpu;
}
std::optional<int> load_cpu_profile(const std::filesystem::path& path,const CpuTopology& topology) {
    try {
        std::ifstream in(path,std::ios::binary|std::ios::ate);
        if(!in)return {};
        const auto length=in.tellg();if(length<0 || length>16384)return {};
        std::string bytes(size_t(length),'\0');in.seekg(0);
        if(!in.read(bytes.data(),std::streamsize(bytes.size())))return {};
        const auto json=Json::parse(bytes,[](int depth,Json::parse_event_t,Json&){if(depth>8)throw std::runtime_error("CPU profile too deep");return true;});
        if(!json.is_object() || !json.contains("version") || !json["version"].is_number_integer() || json["version"]!=profile_version || json.value("machine",std::string())!=topology.fingerprint || !json.contains("cpu") || !json["cpu"].is_number_integer())return {};
        const auto cpu=json["cpu"].get<int64_t>();
        if(cpu<0 || cpu>=63 || !valid_cpu(int(cpu),topology))return {};
        return int(cpu);
    } catch(const std::exception& error) {std::fprintf(stderr,"[cpu-calibration] ignoring invalid profile: %s\n",error.what());return {};}
}
CpuCalibration::CpuCalibration(std::filesystem::path profile) {
    std::promise<CpuCalibrationResult> promise;result_=promise.get_future();
    worker_=std::jthread([path=std::move(profile),promise=std::move(promise)](std::stop_token stop) mutable {
        const auto start=Clock::now();CpuCalibrationResult result;
        try {result=calibrate(path,stop);}
        catch(const std::exception& error) {result.error=error.what();}
        result.elapsed_ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        std::fprintf(stderr,"[cpu-calibration] CPU %d %s %.1f ms%s%s\n",result.cpu,result.cached ? "cached" : "measured",result.elapsed_ms,result.error.empty() ? "" : ": ",result.error.c_str());
        promise.set_value(std::move(result));
    });
}
bool CpuCalibration::ready() const {return result_.valid() && result_.wait_for(std::chrono::seconds(0))==std::future_status::ready;}
CpuCalibrationResult CpuCalibration::get() {return result_.get();}
}
