#include "check.h"
#include "test_server.h"
#include "test_pipe.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
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
// Non-blocking line reader: a blocking ReadFile hangs the test forever when the server has not yet seen a command (reader thread scheduling).
struct LineReader {
    HANDLE h; std::string buf;
    bool next(std::string& line) {
        for(;;) {
            const auto nl=buf.find('\n');
            if(nl!=std::string::npos){ line=buf.substr(0,nl); buf.erase(0,nl+1); return true; }
            DWORD avail=0; if(!PeekNamedPipe(h,nullptr,0,nullptr,&avail,nullptr) || !avail) return false;
            char chunk[4096]; DWORD n=0; if(!ReadFile(h,chunk,std::min<DWORD>(avail,sizeof chunk),&n,nullptr) || !n) return false;
            buf.append(chunk,n);
        }
    }
};
// Pumps the server and reads lines until one contains `key`; "" after the deadline (the CHECK then fails instead of hanging).
static std::string await_line(uc::TestServer& server,LineReader& evt,const std::string& key,uint64_t frame,int ms=10000) {
    const auto limit=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
    std::string line;
    while(std::chrono::steady_clock::now()<limit) {
        server.pump(frame,"{}");
        while(evt.next(line)) if(line.find(key)!=std::string::npos) return line;
        Sleep(1);
    }
    return "";
}
int main() {
    const std::string name="unit-"+std::to_string(GetCurrentProcessId());
    uc::TestPipes pipes(name); FakeSink sink; uc::TestServer server(pipes,sink);
    HANDLE cmd=open_pipe(L"\\\\.\\pipe\\uc-test-"+std::wstring(name.begin(),name.end())+L"-cmd",GENERIC_WRITE);
    HANDLE evt=open_pipe(L"\\\\.\\pipe\\uc-test-"+std::wstring(name.begin(),name.end())+L"-evt",GENERIC_READ);
    CHECK(cmd!=INVALID_HANDLE_VALUE && evt!=INVALID_HANDLE_VALUE);
    // second_client_refused: the single instance is taken.
    CHECK(CreateFileW((L"\\\\.\\pipe\\uc-test-"+std::wstring(name.begin(),name.end())+L"-cmd").c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr)==INVALID_HANDLE_VALUE);
    LineReader lines{evt,{}};
    // Commands are handled in order, so the reply to a following "state" proves the earlier line was seen by the server at that frame.
    write_line(cmd,R"({"id":1,"cmd":"input","hold_frames":2,"kpad":{"x":0.5,"y":-0.5,"buttons":2048}})");
    write_line(cmd,R"({"id":100,"cmd":"state"})");
    CHECK(!await_line(server,lines,"\"id\":100",10).empty());
    CHECK(sink.kpads.empty());                                   // due at frame 11, not 10
    server.pump(11,"{}"); CHECK(sink.kpads.size()==1 && std::get<3>(sink.kpads[0])==0x800);
    server.pump(12,"{}"); CHECK(sink.kpads.size()==1);
    server.pump(13,"{}"); CHECK(sink.kpads.size()==2 && std::get<3>(sink.kpads[1])==0 && std::get<1>(sink.kpads[1])==0.5f);
    sink.consumed_seq=sink.last;
    std::string reply=await_line(server,lines,"\"id\":1,",14);
    CHECK(reply.find(R"("ok":true)")!=std::string::npos && reply.find(R"("consumed_frame":14)")!=std::string::npos);
    write_line(cmd,R"({"id":2,"cmd":"input","kpad":{"x":3,"y":0,"buttons":0}})");
    CHECK(await_line(server,lines,"error",15).find("\"id\":2")!=std::string::npos);
    // peek: hex reply for a readable range, error otherwise.
    write_line(cmd,R"({"id":4,"cmd":"peek","addr":2152422600,"size":2})");
    CHECK(await_line(server,lines,"\"id\":4",15).find(R"("hex":"AABB")")!=std::string::npos);
    write_line(cmd,R"({"id":5,"cmd":"peek","addr":4,"size":2})");
    CHECK(await_line(server,lines,"\"id\":5",15).find("error")!=std::string::npos);
    // draws: requested via pump flag, completed with the summary.
    write_line(cmd,R"({"id":6,"cmd":"draws"})");
    bool wanted=false; for(int i=0;i<10000 && !wanted;++i){ wanted=server.pump(15,"{}").draws; Sleep(1); }
    CHECK(wanted); server.draws_ready("[{\"callsite\":1}]","[1,0]");
    CHECK(await_line(server,lines,"\"id\":6",15).find(R"("draws":[{"callsite":1}],"projection":[1,0])")!=std::string::npos);
    // outgoing_queue_drops_frames_not_replies: the client reads nothing until the queue overflowed.
    for(int i=0;i<10000;++i) pipes.send("{\"type\":\"frame\"}",true);
    pipes.send(R"({"id":99,"ok":true})",false);
    CHECK(!await_line(server,lines,"\"id\":99",15,30000).empty() && pipes.dropped()>0);
    {   // The gamepad devices reach the sink as host events: buttons and axis presses are held and released like keys, a stick position and a gyro rate stay until changed.
        sink.hosts.clear();
        write_line(cmd,R"({"id":30,"cmd":"input","hold_frames":2,"pad_button":{"button":3,"down":true}})");
        write_line(cmd,R"({"id":31,"cmd":"input","pad_stick":{"side":"right","x":0.5,"y":-0.25}})");
        write_line(cmd,R"({"id":32,"cmd":"input","pad_gyro":{"x":1,"y":2}})");
        write_line(cmd,R"({"id":33,"cmd":"input","pad_axis":{"code":11,"down":true}})");
        write_line(cmd,R"({"id":102,"cmd":"state"})");
        CHECK(!await_line(server,lines,"\"id\":102",16).empty());
        server.pump(17,"{}");
        CHECK(sink.hosts.size()==4);
        CHECK(sink.hosts[0].kind==uc::HostEventKind::pad_button && sink.hosts[0].code==3 && sink.hosts[0].value==1);
        CHECK(sink.hosts[1].kind==uc::HostEventKind::pad_stick && sink.hosts[1].code==1 && sink.hosts[1].x==.5f && sink.hosts[1].y==-.25f);
        CHECK(sink.hosts[2].kind==uc::HostEventKind::pad_gyro && sink.hosts[2].x==1.f && sink.hosts[2].y==2.f);
        CHECK(sink.hosts[3].kind==uc::HostEventKind::pad_axis && sink.hosts[3].code==11 && sink.hosts[3].value==1);
        server.pump(18,"{}"); CHECK(sink.hosts.size()==5 && sink.hosts[4].kind==uc::HostEventKind::pad_axis && sink.hosts[4].value==0);   // one frame held
        server.pump(19,"{}"); CHECK(sink.hosts.size()==6 && sink.hosts[5].kind==uc::HostEventKind::pad_button && sink.hosts[5].code==3 && sink.hosts[5].value==0);   // two frames held
        sink.hosts.clear();
        write_line(cmd,R"({"id":34,"cmd":"input","hold_frames":100,"pad_button":{"button":9,"down":true}})");
        write_line(cmd,R"({"id":35,"cmd":"input","pad_stick":{"side":"left","x":1,"y":0}})");
        write_line(cmd,R"({"id":103,"cmd":"state"})");
        CHECK(!await_line(server,lines,"\"id\":103",19).empty());
        server.pump(20,"{}"); CHECK(sink.hosts.size()==2);
        sink.hosts.clear(); server.release_all();   // a disconnect lets go of the held pad button and returns the sticks to rest
        bool released=false,rested=false;
        for(const auto& event:sink.hosts) {
            released|=event.kind==uc::HostEventKind::pad_button && event.code==9 && event.value==0;
            rested|=event.kind==uc::HostEventKind::pad_stick && event.code==0 && event.x==0 && event.y==0;
        }
        CHECK(released && rested);
        sink.hosts.clear();
    }
    // disconnect_releases_inputs: key held, client closes, server releases.
    write_line(cmd,R"({"id":3,"cmd":"input","hold_frames":100,"key":{"code":21,"down":true}})");
    write_line(cmd,R"({"id":101,"cmd":"state"})");
    CHECK(!await_line(server,lines,"\"id\":101",20).empty());   // parsed at frame 20, due at 21
    server.pump(21,"{}");
    CHECK(sink.hosts.size()==1 && sink.hosts[0].value==1);
    CloseHandle(cmd); CloseHandle(evt);
    for(int i=0;i<2000 && !pipes.disconnected();++i) Sleep(5);
    server.release_all(); CHECK(sink.hosts.size()==2 && sink.hosts[1].value==0 && sink.hosts[1].code==21);
    return 0;
}
