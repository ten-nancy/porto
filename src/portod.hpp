#pragma once

#include <util/unix.hpp>

class TEpollLoop;
class TEventQueue;

// master.cpp
int PortodMaster();
bool SanityCheck();

extern TPidFile MasterPidFile;
extern pid_t MasterPid;
extern bool RespawnPortod;

// server.cpp
int Server();

extern TPidFile ServerPidFile;
extern pid_t ServerPid;
extern bool DiscardState;
extern bool ShutdownPortod;
extern std::unique_ptr<TEpollLoop> EpollLoop;
extern std::unique_ptr<TEventQueue> EventQueue;

bool CheckPortoAlive();
void ReopenMasterLog();
void CheckPortoSocket();
