#pragma once
#include "cpu_topology.h"
#include <array>
#include <filesystem>
#include <future>
#include <optional>
#include <thread>
namespace uc {
struct CpuScore {int cpu;std::array<double,3> samples;unsigned scheduling_class=0;unsigned efficiency=0;};
int fastest_cpu(const std::vector<CpuScore>& scores);
std::optional<int> load_cpu_profile(const std::filesystem::path&,const CpuTopology&);
struct CpuCalibrationResult {
    int cpu=-1;
    bool cached=false;
    double elapsed_ms=0;
    std::string error;
};
class CpuCalibration {
public:
    explicit CpuCalibration(std::filesystem::path profile);
    bool ready() const;
    CpuCalibrationResult get();
private:
    std::future<CpuCalibrationResult> result_;
    std::jthread worker_;
};
}
