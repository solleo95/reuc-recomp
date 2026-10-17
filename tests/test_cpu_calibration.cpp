#include "check.h"
#include "cpu_calibration.h"
#include <fstream>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
int main(int argc,char** argv) {
    if(argc==3 && std::string_view(argv[1])=="--calibrate") {
        uc::CpuCalibration calibration(argv[2]);const auto result=calibration.get();
        std::printf("cpu=%d cached=%d elapsed_ms=%.1f error=%s\n",result.cpu,result.cached,result.elapsed_ms,result.error.c_str());
        return result.cpu>=0 ? 0 : 1;
    }
    // An isolated fast sample must not beat consistently faster work.
    CHECK(uc::fastest_cpu({{2,{100,101,500}},{7,{120,119,121}}})==7);
    CHECK(uc::fastest_cpu({})==-1);
    CHECK(uc::fastest_cpu({{2,{0,100,100}}})==-1);
    CHECK(uc::fastest_cpu({{63,{100,100,100}}})==-1);
    // A sub-percent probe lead is noise; prefer the scheduling hint among close candidates.
    CHECK(uc::fastest_cpu({{26,{52390.9,45460.8,50883.3},7},{28,{51150.9,37808.7,52989.8},0}})==26);
    CHECK(uc::fastest_cpu({{28,{51150.9,37808.7,52989.8},0},{26,{52390.9,45460.8,50883.3},7}})==26);
    // The hint cannot override a clearly faster measurement or resurrect invalid samples.
    CHECK(uc::fastest_cpu({{2,{80,80,80},14},{7,{100,100,100},7}})==7);
    CHECK(uc::fastest_cpu({{2,{0,100,100},14},{7,{100,100,100},7}})==7);
    CHECK(uc::fastest_cpu({{2,{100,100,100},0},{7,{102,102,102},0}})==7);
    // SMT indices need not be adjacent; unrelated cores stay available.
    std::vector<uc::CpuCore> cores={{2,0x84,0},{0,0x09,0}};
    CHECK(uc::cpu_worker_mask(cores,2,0xff)==0x7f);
    CHECK(uc::cpu_worker_mask(cores,7,0xff)==0xfb);
    CHECK(uc::cpu_worker_mask(cores,5,0xff)==0xff);
    CHECK(uc::cpu_worker_mask(cores,2,0x04)==0x04);
    const auto dir=std::filesystem::temp_directory_path()/std::filesystem::path("uc-cpu-test-"+std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    const auto path=dir/"profile.json";
    uc::CpuTopology topology{"machine-a",cores};
    topology.cores[0].cpu=7;
    CHECK(!uc::load_cpu_profile(path,topology));
    {std::ofstream out(path);out<<R"({"version":2,"machine":"machine-a","cpu":7})";}
    CHECK(uc::load_cpu_profile(path,topology)==7);
    topology.fingerprint="machine-b";
    CHECK(!uc::load_cpu_profile(path,topology));
    topology.fingerprint="machine-a";
    for(const auto* value:{R"({"version":1,"machine":"machine-a","cpu":7})",R"({"version":2,"machine":"machine-a","cpu":5})",R"({"version":2,"machine":"machine-a","cpu":2.5})",R"({"version":2,"machine":"machine-a","cpu":4294967303})","{"}) {
        {std::ofstream out(path);out<<value;}
        CHECK(!uc::load_cpu_profile(path,topology));
    }
    const auto machine=uc::cpu_topology();
    if(!machine.cores.empty()) {
        // Two launchers share one measurement; the waiter reads its atomic result.
        uc::CpuCalibration first(path),second(path);
        const auto a=first.get(),b=second.get();
        CHECK(a.cpu>=0 && a.cpu==b.cpu && a.cached!=b.cached);
        CHECK(a.error.empty() && b.error.empty());
        uc::CpuCalibration warm(path);const auto cached=warm.get();
        CHECK(cached.cached && cached.cpu==a.cpu);
        const auto cancel_path=dir/"cancel.json";
        {uc::CpuCalibration canceled(cancel_path);}
        CHECK(!std::filesystem::exists(cancel_path));
        // A permanently invalid directory must complete with a usable fallback.
        uc::CpuCalibration unwritable(path/"child.json");const auto fallback=unwritable.get();
        CHECK(fallback.cpu==-1 && !fallback.error.empty());
    }
    std::filesystem::remove_all(dir);
    return 0;
}
