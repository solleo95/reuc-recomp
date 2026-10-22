#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <vector>

namespace uc {
// A graphics pipeline is fully described by this key (blend/cull word, TEV specialization constants, depth word, topology).
using PipelineKey=std::array<uint32_t,70>;

// Key file: {magic, version, key size} followed by raw keys. Replaying it at startup compiles pipelines before the game first needs them.
inline constexpr uint32_t kPipelineKeysHeader[3]={0x4B504355u,1u,uint32_t(sizeof(PipelineKey))};

inline bool valid_pipeline_key(const PipelineKey& key,bool logic_ops) {
    const bool topology=key[69]==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || key[69]==VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    return topology && key[1]<=16 && key[67]<=1 && key[68]<32 && (key[0]>>16)==0 && ((key[0]&3)!=2 || logic_ops);
}

// The part of a key the ubershader still needs a separate pipeline for (blend/cull, depth, topology); the TEV words and the
// destination-alpha pass are read from the draw at run time.
inline PipelineKey uber_key(PipelineKey key) {
    std::fill(key.begin()+1,key.begin()+68,0u);
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

inline std::vector<PipelineKey> load_pipeline_keys(const std::filesystem::path& path) {
    std::vector<PipelineKey> keys;
    std::unique_ptr<FILE,int(*)(FILE*)> file(std::fopen(path.string().c_str(),"rb"),std::fclose);
    uint32_t header[3];
    if(!file || std::fread(header,sizeof header,1,file.get())!=1 || !std::equal(header,header+3,kPipelineKeysHeader)) return keys;
    for(PipelineKey key; std::fread(key.data(),sizeof key,1,file.get())==1;) keys.push_back(key);   // a torn trailing record is dropped
    return keys;
}

// Appends one key, creating the file (or replacing an unreadable one) with a fresh header. Returns false when it cannot be written.
inline bool append_pipeline_key(const std::filesystem::path& path,const PipelineKey& key) {
    std::unique_ptr<FILE,int(*)(FILE*)> probe(std::fopen(path.string().c_str(),"rb"),std::fclose);
    uint32_t header[3];
    const bool intact=probe && std::fread(header,sizeof header,1,probe.get())==1 && std::equal(header,header+3,kPipelineKeysHeader);
    probe.reset();
    std::unique_ptr<FILE,int(*)(FILE*)> file(std::fopen(path.string().c_str(),intact ? "ab" : "wb"),std::fclose);
    if(!file) return false;
    if(!intact && std::fwrite(kPipelineKeysHeader,sizeof kPipelineKeysHeader,1,file.get())!=1) return false;
    return std::fwrite(key.data(),sizeof key,1,file.get())==1 && std::fflush(file.get())==0;
}
}
