#pragma once
// Internal: Streamline headers and the entry points loaded from sl.interposer.dll. Only renderer/streamline*.cpp and dlss_*.cpp include it.
#include <vulkan/vulkan.h>
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <sl_helpers.h>
#include <sl_helpers_vk.h>

namespace uc {
struct StreamlineApi {
    PFun_slInit* init=nullptr;
    PFun_slShutdown* shutdown=nullptr;
    PFun_slIsFeatureSupported* is_feature_supported=nullptr;
    PFun_slGetFeatureFunction* get_feature_function=nullptr;
    PFun_slGetNewFrameToken* get_new_frame_token=nullptr;
    PFun_slSetTagForFrame* set_tag_for_frame=nullptr;
    PFun_slSetConstants* set_constants=nullptr;
    PFun_slEvaluateFeature* evaluate_feature=nullptr;
    PFun_slFreeResources* free_resources=nullptr;
    PFN_vkGetInstanceProcAddr get_instance_proc=nullptr;   // SL's: vulkan-1 addresses except for the intercepted calls
    PFN_vkGetDeviceProcAddr get_device_proc=nullptr;
    sl::FrameToken* frame=nullptr;   // this guest frame's token (Streamline::begin_frame): DLSS constants, frame generation and Reflex share it
};
}
