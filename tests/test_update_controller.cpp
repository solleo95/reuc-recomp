#include "update.h"
#include "check.h"
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <fstream>
using namespace uc::update;
using namespace uc::launcher;
template<class T> T ok(Result<T> result) {CHECK(!failed(result));return std::get<T>(std::move(result));}
struct Fake:Transport {
    std::atomic<unsigned> checks{0},downloads{0},status{200};
    std::atomic<bool> hold_check{false},hold_download{false};
    std::string metadata,bytes;
    Result<HttpResponse> get_metadata(std::stop_token stop) override {
        ++checks;while(hold_check && !stop.stop_requested()) Sleep(1);
        return HttpResponse{status.load(),metadata};
    }
    Status download(const Release&,const DownloadSink& sink,std::stop_token stop) override {
        ++downloads;auto status=sink(std::span<const char>(bytes.data(),bytes.size()/2));
        if(failed(status)) return status;
        while(hold_download && !stop.stop_requested()) Sleep(1);
        if(stop.stop_requested()) return Error{ErrorCode::cancelled,"cancelled"};
        return sink(std::span<const char>(bytes.data()+bytes.size()/2,bytes.size()-bytes.size()/2));
    }
};
void settle(UpdateController& controller) {
    for(int i=0;i<1000 && controller.busy();++i) {controller.poll();Sleep(1);}
    CHECK(!controller.busy());
}
int main() {
    auto root=fs::temp_directory_path()/("uc-controller-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    fs::create_directories(root/".uc-update");
    auto fixture=ok(read_text(fs::path(UC_SOURCE_DIR)/"tests/fixtures/update/release-v1.json",max_manifest_size));
    auto installed=ok(parse_manifest(fixture));installed.version={0,0,0};
    mz_zip_archive archive{};CHECK(mz_zip_writer_init_heap(&archive,0,0));
    CHECK(mz_zip_writer_add_mem(&archive,"release.json",fixture.data(),fixture.size(),MZ_BEST_SPEED));
    for(auto name:{"uc.exe","uc_launcher.exe","uc_updater.exe"}) CHECK(mz_zip_writer_add_mem(&archive,name,"",0,0));
    void* data=nullptr;size_t size=0;CHECK(mz_zip_writer_finalize_heap_archive(&archive,&data,&size));
    std::string bytes(static_cast<const char*>(data),size);mz_free(data);CHECK(mz_zip_writer_end(&archive));
    {std::ofstream zip(root/"fixture.zip",std::ios::binary);zip.write(bytes.data(),bytes.size());}
    auto hash=ok(sha256_file(root/"fixture.zip"));
    auto make=[&] {
        auto transport=std::make_unique<Fake>();transport->bytes=bytes;
        transport->metadata=nlohmann::json{{"tag_name","0.0.1"},{"draft",false},{"prerelease",false},{"body","Release notes"},
            {"assets",nlohmann::json::array({{{"name","UC-Windows-0.0.1.zip"},{"state","uploaded"},{"size",size},
                {"digest","sha256:"+hash},{"browser_download_url","https://github.com/solleo95/reuc-recomp/releases/download/0.0.1/UC-Windows-0.0.1.zip"}}})}}.dump();
        return transport;
    };
    {
        auto fake=make();auto* ptr=fake.get();UpdateController controller(root,std::move(fake));
        controller.start(std::nullopt);controller.start(installed);CHECK(ptr->checks==0);CHECK(controller.state()==UpdateState::disabled);
    }
    {
        auto fake=make();auto* ptr=fake.get();ptr->status=404;UpdateController controller(root,std::move(fake));
        controller.start(installed);settle(controller);CHECK(controller.state()==UpdateState::disabled);CHECK(!controller.blocks_game_launch());
    }
    {
        auto fake=make();auto* ptr=fake.get();ptr->hold_check=true;UpdateController controller(root,std::move(fake));
        controller.start(installed);controller.start(installed);controller.postpone();settle(controller);
        CHECK(ptr->checks<=1);CHECK(controller.state()==UpdateState::disabled);CHECK(!controller.close_requested());
    }
    for(const auto& digest:{nlohmann::json(nullptr),nlohmann::json("invalid")}) {
        auto fake=make();auto* ptr=fake.get();
        auto metadata=nlohmann::json::parse(ptr->metadata);metadata["assets"][0]["digest"]=digest;ptr->metadata=metadata.dump();
        UpdateController controller(root,std::move(fake));controller.start(installed);settle(controller);
        CHECK(controller.state()==UpdateState::failed);CHECK(controller.error());
        CHECK(controller.error()->detail=="Release SHA-256 is unavailable");
        CHECK(!controller.release());CHECK(!controller.blocks_game_launch());
        controller.accept();CHECK(ptr->downloads==0);controller.postpone();CHECK(controller.state()==UpdateState::disabled);
    }
    {
        auto fake=make();auto* ptr=fake.get();std::atomic<unsigned> helpers{0};bool fail_helper=true;
        UpdateController controller(root,std::move(fake),[&](const fs::path&,const Package&)->Result<HelperProcess> {
            ++helpers;if(fail_helper) return Error{ErrorCode::startup,"fixture helper failed"};return HelperProcess{};
        });
        controller.start(installed);controller.start(installed);settle(controller);CHECK(ptr->checks==1);CHECK(controller.state()==UpdateState::available);
        controller.accept(true);CHECK(ptr->downloads==0);CHECK(!controller.blocks_game_launch());
        ptr->hold_download=true;controller.accept();controller.accept();CHECK(controller.blocks_game_launch());
        for(int i=0;i<1000 && ptr->downloads==0;++i) Sleep(1);
        CHECK(ptr->downloads==1);controller.cancel();controller.accept();settle(controller);
        CHECK(controller.state()==UpdateState::available);CHECK(!controller.blocks_game_launch());CHECK(helpers==0);
        ptr->hold_download=false;controller.accept();settle(controller);
        CHECK(controller.state()==UpdateState::failed);CHECK(!controller.close_requested());CHECK(!controller.blocks_game_launch());
        fail_helper=false;controller.accept();controller.accept();settle(controller);
        CHECK(helpers==2);CHECK(ptr->downloads==3);CHECK(controller.close_requested());
        controller.cancel();CHECK(!controller.close_requested());CHECK(!controller.blocks_game_launch());
    }
    {
        auto fake=make();auto* ptr=fake.get();ptr->hold_download=true;UpdateController controller(root,std::move(fake));
        controller.start(installed);settle(controller);controller.accept();
        for(int i=0;i<1000 && ptr->downloads==0;++i) Sleep(1);
        CHECK(ptr->downloads==1);
    }
    CHECK(fs::is_empty(root/".uc-update"));fs::remove_all(root);
}
