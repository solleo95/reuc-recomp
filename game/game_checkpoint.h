#pragma once
#include "native_input.h"
#include "savefs.h"
#include "sched.h"
#include "rollback_memory.h"
#include <span>

namespace uc {
struct GameCheckpoint;
std::shared_ptr<const GameCheckpoint> game_checkpoint(RollbackMemory&,Sched&,SaveFs&,std::span<NativeInput* const> inputs,bool verify_writes=false);
void game_restore(const GameCheckpoint&,RollbackMemory&,Sched&,SaveFs&);
uint64_t game_checkpoint_hash(const GameCheckpoint&);
// Desync diagnostic (UC_NET_HASH_DETAIL): scheduler hash, virtual clock and the RAM chunk hashes of RollbackMemory::chunk_hashes as one log line.
std::string game_checkpoint_detail(const GameCheckpoint&);
// Desync diagnostic: the raw guest RAM of a checkpoint into a file (false when the file cannot be written).
bool game_checkpoint_dump(const GameCheckpoint&,const std::string& path);
void game_checkpoint_commit_saves(const GameCheckpoint&,SaveFs&);
// Harness diagnostic: reports the first guest-memory difference after replay.
std::string game_checkpoint_difference(const GameCheckpoint&,const GameCheckpoint&);
void install_rollback_probe(Sched&,SaveFs&,NativeInput&,uint64_t first_retrace,unsigned iterations);
}
