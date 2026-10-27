#include "check.h"
#include "native_tev.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

// A stand-in for smoke.spec.hlsl: the real file is the Slang output with UC_TEV(i) calls; this one reads two words the same way.
constexpr const char* kShader=R"(float4 fragmentMain() : SV_Target { return float4(float(UC_TEV(0)), float(UC_TEV(1)), float(UC_TEV(66)), 1.0); })";

int main() {
    uc::TevWords words{}; words[0]=2; words[1]=0xE4E4E4E4u; words[66]=1;
    const auto prelude=uc::TevShaders::prelude(words);
    CHECK(prelude.find("uc_tev[67]")!=std::string::npos && prelude.find("2u,3840206052u,")!=std::string::npos && prelude.find("#define UC_TEV(i) uc_tev[i]")!=std::string::npos);

    const auto folder=std::filesystem::temp_directory_path()/"uc_test_native_tev";
    std::filesystem::remove_all(folder); std::filesystem::create_directories(folder);
    for(const char* name:{"smoke.spec.hlsl","smoke.motion.spec.hlsl"}) std::ofstream(folder/name)<<kShader;
    int finished=0;
    const auto finish=[&](const uc::TevShaders::Bytecode& code) -> std::shared_ptr<void> { ++finished; return std::make_shared<size_t>(code->size()); };
    {
        uc::TevShaders shaders(folder,folder,finish,true);
        CHECK(shaders.available());
        const auto* entry=shaders.get(0,words);
        CHECK(entry && entry->code && !entry->code->empty() && entry->object);
        CHECK(*static_cast<size_t*>(entry->object.get())==entry->code->size() && finished==1);
        CHECK(shaders.get(0,words)==entry);                                // the same entry again: nothing compiles twice
        CHECK(shaders.get(1,words) && finished==2);                        // the other variant is its own shader
        auto other=words; other[0]=3;
        const auto* changed=shaders.get(0,other);
        CHECK(changed && changed->code && *changed->code!=*entry->code);   // the words are constants of the shader
        CHECK(!shaders.get(2,words));                                      // no such variant
    }
    CHECK(std::filesystem::file_size(folder/"dx_tev_cache.bin")>16);
    {   // a second run finds the bytecode in the cache file
        finished=0;
        uc::TevShaders shaders(folder,folder,finish,true);
        const auto* entry=shaders.get(0,words);
        CHECK(entry && entry->code && !entry->code->empty() && finished==1);
    }
    // HLSL that does not compile: the draw keeps the ubershader (null), and nothing is stored for it
    std::ofstream(folder/"smoke.spec.hlsl")<<"float4 fragmentMain() : SV_Target { return nonsense; }";
    std::filesystem::remove(folder/"dx_tev_cache.bin");
    {
        uc::TevShaders shaders(folder,folder,finish,true);
        CHECK(shaders.available() && !shaders.get(0,words));
        CHECK(shaders.get(1,words));
    }
    // Missing HLSL: unavailable, not an error
    std::filesystem::remove(folder/"smoke.motion.spec.hlsl");
    CHECK(!uc::TevShaders(folder,folder,finish,true).available());
    std::filesystem::remove_all(folder);
    return 0;
}
