#pragma once
#include <vulkan/vulkan.h>
#include "decal_bias.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <thread>
#include <vector>

namespace uc {
// Fixed-function/TEV words keep their original offsets; words70..85 specialize the indirect commands.
using PipelineKey=std::array<uint32_t,96>;

inline bool discard_zero_alpha(const PipelineKey& key) {
    // These draws preserve destination RGBA at alpha0 and write no auxiliary attachment or depth.
    return (key[0]&255u)==(1u|(4u<<2)|(5u<<5)) && (key[68]&17u)!=17u && key[67]==0 &&
           key[95]==0 && ((key[90]|key[91]|key[92]|key[93])&2u)==0;
}

inline uint32_t fog_control(uint32_t word) {
    const auto control=(word>>20)&15u;
    return (control>>1)!=0 ? control : 0u;
}

// Key file: {magic, version, key size} followed by raw keys. Replaying it at startup compiles pipelines before the game first needs them.
inline constexpr uint32_t kPipelineKeysHeader[3]={0x4B504355u,1u,uint32_t(sizeof(PipelineKey))};

inline bool valid_pipeline_key(const PipelineKey& key,bool logic_ops) {
    const bool topology=key[69]==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || key[69]==VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    return topology && key[1]<=16 && key[67]<=1 && key[68]<32 && key[95]<16 && (key[0]>>16)==0 && ((key[0]&3)!=2 || logic_ops);
}

// Cull mode and the depth test, write and compare op are dynamic state (Vulkan 1.3), set from the draw's full key; a pipeline only keeps
// whether the draw writes depth (the motion and G-buffer masks follow it). Pipelines, the seed's closure and the recorded file use this form.
// Which of the 8 lights a lighting channel uses (key words 90..93, channel control bits 2..5 and 11..14): the shader reads it from the draw, because every
// combination of lights an object can be lit with would otherwise be a pipeline of its own (537 instead of 140 keys in the first chapter).
inline constexpr uint32_t kLightMaskBits=(15u<<2)|(15u<<11);

inline PipelineKey static_key(PipelineKey key) {
    key[0]&=~(3u<<14);
    key[68]=(key[68]&17)==17 ? 17u : 0u;
    for(size_t channel=90;channel<94;++channel) key[channel]&=~kLightMaskBits;
    return key;
}

inline bool decal_depth_bias(const PipelineKey& key) { return decal_depth_bias(key[0]&3,key[68]); }

inline VkCullModeFlags cull_mode(const PipelineKey& key) {
    constexpr VkCullModeFlags culls[]={VK_CULL_MODE_NONE,VK_CULL_MODE_BACK_BIT,VK_CULL_MODE_FRONT_BIT,VK_CULL_MODE_FRONT_AND_BACK};
    return culls[(key[0]>>14)&3];
}

// The part of a key the ubershader still needs a separate pipeline for (blend/cull, depth, topology); the TEV words and the
// destination-alpha pass are read from the draw at run time.
inline PipelineKey uber_key(PipelineKey key) {
    std::fill(key.begin()+1,key.begin()+68,0u);
    std::fill(key.begin()+70,key.end(),0u);
    return key;
}

// Every ubershader key built from the blend/cull words, depth words and topologies that occur in `keys`: a key not recorded yet is mostly a
// new TEV setup over fixed-function state seen before, so these cover it with a ready pipeline.
inline std::vector<PipelineKey> uber_key_closure(const std::vector<PipelineKey>& keys) {
    std::set<uint32_t> blends,depths,topologies;
    for(const auto& key:keys) { blends.insert(key[0]); depths.insert(key[68]); topologies.insert(key[69]); }
    std::vector<PipelineKey> result;
    for(auto blend:blends) for(auto depth:depths) for(auto topology:topologies) { PipelineKey key{}; key[0]=blend; key[68]=depth; key[69]=topology; result.push_back(key); }
    return result;
}

// Write mask of the motion attachment (upscaling): only draws that write depth (compare + update, key word 68) write their vectors.
inline VkColorComponentFlags motion_write_mask(const PipelineKey& key) {
    return (key[68]&17)==17 ? VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT : 0;
}

// Write mask of the G-buffer attachment: the same draws that write motion vectors, i.e. the ones that define a visible surface.
inline VkColorComponentFlags gbuffer_write_mask(const PipelineKey& key) {
    return (key[68]&17)==17 ? VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT : 0;
}

// Whether a draw writes the RT material-record and albedo attachments: the G-buffer's draws, except logic-op ones (the op would apply to the record index).
inline bool rt_material_write(const PipelineKey& key) {
    return gbuffer_write_mask(key) && (key[0]&3)!=2;
}

// `legacy` (optional) tells whether the file has shorter records than the current key: its keys are zero-extended, so the words added since never match a real draw.
inline std::vector<PipelineKey> load_pipeline_keys(const std::filesystem::path& path,bool* legacy=nullptr) {
    std::vector<PipelineKey> keys;
    if(legacy) *legacy=false;
    std::unique_ptr<FILE,int(*)(FILE*)> file(std::fopen(path.string().c_str(),"rb"),std::fclose);
    uint32_t header[3];
    if(!file || std::fread(header,sizeof header,1,file.get())!=1 || header[0]!=kPipelineKeysHeader[0] || header[1]!=kPipelineKeysHeader[1] ||
       (header[2]!=sizeof(PipelineKey) && header[2]!=95*sizeof(uint32_t) && header[2]!=90*sizeof(uint32_t) && header[2]!=89*sizeof(uint32_t) && header[2]!=86*sizeof(uint32_t) && header[2]!=70*sizeof(uint32_t))) return keys;
    // Old seeds warm direct-stage variants; draws with indirect commands get distinct keys.
    if(legacy) *legacy=header[2]!=sizeof(PipelineKey);
    for(PipelineKey key{}; std::fread(key.data(),header[2],1,file.get())==1;) keys.push_back(key);   // a torn trailing record is dropped
    return keys;
}

// Appends one key, creating the file (or replacing an unreadable one) with a fresh header. Returns false when it cannot be written.
inline bool append_pipeline_key_once(const std::filesystem::path& path,const PipelineKey& key) {
    std::unique_ptr<FILE,int(*)(FILE*)> probe(std::fopen(path.string().c_str(),"rb"),std::fclose);
    uint32_t header[3];
    const bool intact=probe && std::fread(header,sizeof header,1,probe.get())==1 && std::equal(header,header+3,kPipelineKeysHeader);
    probe.reset();
    std::error_code error;
    const auto links=std::filesystem::hard_link_count(path,error);
    if(intact && error) return false;
    const bool links_known=!error;
    // The file is written in place (a missing one is created, an unreadable one rewritten; a torn write is an unreadable file the next time) unless it has other names:
    // those go through a temporary file and a rename, so the data under the other names does not change. (A rename of a file that was just written can fail for a while
    // under a virus scanner, which is why the plain cases avoid it.)
    const bool missing=!std::filesystem::exists(path,error);
    const bool replace=!missing && links_known && links>1;
    auto temporary=path; temporary+=".tmp";
    if(intact && replace) {
        std::filesystem::copy_file(path,temporary,std::filesystem::copy_options::overwrite_existing,error);
        if(error) return false;
    }
    const auto& destination=replace ? temporary : path;
    std::unique_ptr<FILE,int(*)(FILE*)> file(std::fopen(destination.string().c_str(),intact ? "ab" : "wb"),std::fclose);
    if(!file) return false;
    if(!intact && std::fwrite(kPipelineKeysHeader,sizeof kPipelineKeysHeader,1,file.get())!=1) return false;
    if(std::fwrite(key.data(),sizeof key,1,file.get())!=1 || std::fflush(file.get())!=0) return false;
    file.reset();
    if(replace) {
        std::filesystem::rename(temporary,path,error);
        if(error) return false;
    }
    return true;
}
// A virus scanner or an indexer may hold the file for a moment right after it is written (Windows: a sharing violation on the temporary file or on the rename): the
// write is tried again a few times before the key is given up. A path that cannot be written at all costs about 0.15 s.
inline bool append_pipeline_key(const std::filesystem::path& path,const PipelineKey& key) {
    for(int attempt=0;attempt<6;++attempt) {
        if(append_pipeline_key_once(path,key)) return true;
        if(attempt<5) std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return false;
}
}
