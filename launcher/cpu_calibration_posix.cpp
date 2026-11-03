// Linux: no CPU calibration. The Windows launcher times each core and tells the game which one to use; on Linux the game pins its guest thread itself by default (UC_NO_GUEST_PIN=1 turns that off),
// so the launcher's answer is "no choice" (-1), ready at once.
#include "cpu_calibration.h"

namespace uc {
CpuCalibration::CpuCalibration(std::filesystem::path) {
    std::promise<CpuCalibrationResult> promise;
    CpuCalibrationResult result;
    result.cached = true;
    promise.set_value(result);
    result_ = promise.get_future();
}
bool CpuCalibration::ready() const { return true; }
CpuCalibrationResult CpuCalibration::get() { return result_.get(); }
}  // namespace uc
