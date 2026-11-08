#pragma once
struct PlatformContext;

// Crash recovery: reload persisted ops into the coordinator. Fanout is
// deliberately not re-issued on recovery; preparing ops keep their deadline
// for the sweeper, committed ops serve as rollback snapshots.
void restore_ops(PlatformContext& ctx);

// Barrier timeout sweeper: expire preparing barriers, replay rollbacks that
// landed in the crash window, and GC terminal ops plus soft-deleted walls.
void start_barrier_sweeper(PlatformContext& ctx);
