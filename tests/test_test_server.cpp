#include "check.h"
#include "test_server.h"
#include "test_pipe.h"
#include <windows.h>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
struct FakeSink : uc::TestInputSink {
    std::vector<std::tuple<uint64_t,float,float,uint32_t>> kpads; std::vector<uc::HostEvent> hosts; uint64_t consumed_seq=0, last=0;
    bool kpad(uint64_t s,float x,float y,uint32_t b) override { kpads.emplace_back(s,x,y,b); last=s; return true; }
    bool host(uint64_t s,const uc::HostEvent& e) override { hosts.push_back(e); last=s; return true; }
    uint64_t consumed() override { return consumed_seq; }
    std::string peek(uint32_t address,uint32_t size) override { return address==0x804B5CC8u && size==2 ? "AABB" : ""; }
};
static HANDLE open_pipe(const std::wstring& name,DWORD access) {
    for(int i=0;i<200;++i){ HANDLE h=CreateFileW(name.c_str(),access,0,nullptr,OPEN_EXISTING,0,nullptr); if(h!=INVALID_HANDLE_VALUE) return h; Sleep(10);} return INVALID_HANDLE_VALUE;
}
static void write_line(HANDLE h,const std::string& s){ std::string l=s+"\n"; DWORD n=0; CHECK(WriteFile(h,l.data(),DWORD(l.size()),&n,nullptr) && n==l.size()); }
static std::string read_line(HANDLE h){ std::string out; char c; DWORD n=0; while(ReadFile(h,&c,1,&n,nullptr) && n==1 && c!='\n') out+=c; return out; }
int main() {
    const std::string name="unit-"+std::to_string(GetCurrentProcessId());
    uc::TestPipes pipes(name); FakeSink sink; uc::TestServer server(pipes,sink);
    HANDLE cmd=open_pipe(L"\\\\.\\pipe\\uc-test-"+std::wstring(name.begin(),name.end())+L"-cmd",GENERIC_WRITE);
    HANDLE evt=open_pipe(L"\\\\.\\pipe\\uc-test-"+std::wstring(name.begin(),name.end())+L"-evt",GENERIC_READ);
    CHECK(cmd!=INVALID_HANDLE_VALUE && evt!=INVALID_HANDLE_VALUE);
    // second_client_refused: the single instance is taken.
    CHECK(CreateFileW((L"\\\\.\\pipe\\uc-test-"+std::wstring(name.begin(),name.end())+L"-cmd").c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr)==INVALID_HANDLE_VALUE);
    write_line(cmd,R"({"id":1,"cmd":"input","hold_frames":2,"kpad":{"x":0.5,"y":-0.5,"buttons":2048}})");
    for(int i=0;i<100 && sink.kpads.empty();++i){ server.pump(10,"{\"type\":\"frame\",\"n\":10}"); Sleep(5); }
    CHECK(sink.kpads.empty());                                   // due at frame 11, not 10
    server.pump(11,"{}"); CHECK(sink.kpads.size()==1 && std::get<3>(sink.kpads[0])==0x800);
    server.pump(12,"{}"); CHECK(sink.kpads.size()==1);
    server.pump(13,"{}"); CHECK(sink.kpads.size()==2 && std::get<3>(sink.kpads[1])==0 && std::get<1>(sink.kpads[1])==0.5f);
    sink.consumed_seq=sink.last; server.pump(14,"{\"type\":\"frame\",\"n\":14}");
    std::string reply; for(int i=0;i<20 && reply.find("\"id\":1")==std::string::npos;++i) reply=read_line(evt);
    CHECK(reply.find(R"("ok":true)")!=std::string::npos && reply.find(R"("consumed_frame":14)")!=std::string::npos);
    write_line(cmd,R"({"id":2,"cmd":"input","kpad":{"x":3,"y":0,"buttons":0}})");
    server.pump(15,"{}"); for(int i=0;i<20 && reply.find("\"id\":2")==std::string::npos;++i){ server.pump(15,"{}"); reply=read_line(evt); }
    CHECK(reply.find("error")!=std::string::npos);
    // peek: hex reply for a readable range, error otherwise. Pump first so the reader thread has delivered the line before we block on the pipe.
    const auto await_reply=[&](int id){ for(int i=0;i<50;++i){ server.pump(15,"{}"); Sleep(2); } std::string line; for(int i=0;i<50 && line.find("\"id\":"+std::to_string(id))==std::string::npos;++i) line=read_line(evt); return line; };
    write_line(cmd,R"({"id":4,"cmd":"peek","addr":2152422600,"size":2})");
    CHECK(await_reply(4).find(R"("hex":"AABB")")!=std::string::npos);
    write_line(cmd,R"({"id":5,"cmd":"peek","addr":4,"size":2})");
    CHECK(await_reply(5).find("error")!=std::string::npos);
    // draws: requested via pump flag, completed with the summary.
    write_line(cmd,R"({"id":6,"cmd":"draws"})");
    bool wanted=false; for(int i=0;i<50 && !wanted;++i){ wanted=server.pump(15,"{}").draws; Sleep(2); }
    CHECK(wanted); server.draws_ready("[{\"callsite\":1}]","[1,0]");
    { std::string line; for(int i=0;i<50 && line.find("\"id\":6")==std::string::npos;++i) line=read_line(evt); CHECK(line.find(R"("draws":[{"callsite":1}],"projection":[1,0])")!=std::string::npos); }
    // outgoing_queue_drops_frames_not_replies
    for(int i=0;i<10000;++i) pipes.send("{\"type\":\"frame\"}",true);
    pipes.send(R"({"id":99,"ok":true})",false);
    bool got=false; for(int i=0;i<20000 && !got;++i) got=read_line(evt).find("\"id\":99")!=std::string::npos;
    CHECK(got && pipes.dropped()>0);
    // disconnect_releases_inputs: key held, client closes, server releases.
    write_line(cmd,R"({"id":3,"cmd":"input","hold_frames":100,"key":{"code":21,"down":true}})");
    for(int i=0;i<50;++i){ server.pump(20,"{}"); Sleep(5); }    // parsed at frame 20, due at 21
    server.pump(21,"{}");
    CHECK(sink.hosts.size()==1 && sink.hosts[0].value==1);
    CloseHandle(cmd); CloseHandle(evt);
    for(int i=0;i<100 && !pipes.disconnected();++i) Sleep(5);
    server.release_all(); CHECK(sink.hosts.size()==2 && sink.hosts[1].value==0 && sink.hosts[1].code==21);
    return 0;
}
