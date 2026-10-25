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
void game_checkpoint_commit_saves(const GameCheckpoint&,SaveFs&);
// Harness diagnostic: reports the first guest-memory difference after replay.
std::string game_checkpoint_difference(const GameCheckpoint&,const GameCheckpoint&);
void install_rollback_probe(Sched&,SaveFs&,NativeInput&,uint64_t first_retrace,unsigned iterations);
}
