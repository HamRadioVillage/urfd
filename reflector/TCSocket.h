#pragma once

#include <string>
#include <cstdint>
#include <mutex>
#include <vector>
#include <queue>
#include <memory>
#include <thread>
#include <condition_variable>
#include <map>
#include <atomic>
#include <set>
#include <sstream>
#include <nng/nng.h>
#include <nng/protocol/pair1/pair.h>

#include "TCPacketDef.h"

// Specialized thread-safe queue for STCPacket by value, avoiding template conflict
class CTCPacketQueue {
    std::queue<STCPacket> q;
    std::mutex m;
    std::condition_variable cv;
public:
    void Push(const STCPacket& p) {
        std::lock_guard<std::mutex> l(m);
        q.push(p);
        cv.notify_one();
    }
    bool Pop(STCPacket& p, int ms) {
        std::unique_lock<std::mutex> l(m);
        // Wait up to ms if queue is empty
        if (q.empty()) {
            if (ms <= 0) return false;
            // wait_for returns false if timeout, true if predicate is true
            if (!cv.wait_for(l, std::chrono::milliseconds(ms), [this]{ return !q.empty(); })) {
                return false; // timeout
            }
        }
        
        p = q.front();
        q.pop();
        return true;
    }
};

class CTCSocket
{
public:
	CTCSocket();
	virtual ~CTCSocket();

	virtual bool Open(const std::string &address, const std::string &modules, uint16_t port) = 0;
	void Close(); 
	void Close(char module); 

	bool Send(const STCPacket *packet);

	bool IsConnected(char module) const;
	bool IsConnected() const { return m_Connected; }
    int GetFD(char module) const; // Legacy compat: returns 1 if connected, -1 if not
	unsigned SecondsDisconnected() const; // 0 while a peer is connected
    
    std::string GetAndClearStats();

protected:
    nng_socket m_Sock;
    std::thread m_Thread;
    std::atomic<bool> m_Running;
    std::atomic<bool> m_Connected; // true while there is a peer on the other end
	std::string m_Modules;

	// pair1 takes one peer at a time, and NNG tells us when it comes and goes
	mutable std::mutex m_PipeMutex;
	nng_pipe m_Pipe;
	std::string m_PeerAddr;
	std::atomic<int64_t> m_LastRxMs;    // when anything last arrived from the peer
	std::atomic<int64_t> m_DownSinceMs; // when the peer last went away
	std::atomic<bool> m_PeerPings;      // the peer sends keepalives, so its silence means it is gone

    // Per-module input queues
    std::map<char, std::shared_ptr<CTCPacketQueue>> m_Queues;
    // Client queue (receives all)
    // Client queue (receives all)
    std::shared_ptr<CTCPacketQueue> m_ClientQueue;

    // Track seen modules for logging
    std::set<char> m_SeenModules;
    
    // Packet counters
    std::map<char, int> m_PacketCounts;
    std::mutex m_StatsMutex;

    void Dispatcher();
	bool OpenSocket();
	void SendPing();
	void CheckLiveness(int64_t now);
	static void PipeEvent(nng_pipe pipe, nng_pipe_ev ev, void *arg);
};

class CTCServer : public CTCSocket
{
public:
	CTCServer() : CTCSocket() {}
	~CTCServer() {}
	bool Open(const std::string &address, const std::string &modules, uint16_t port);
	bool Receive(char module, STCPacket *packet, int ms);
	bool AnyAreClosed() const;
	bool Accept(); // Checks NNG state
};

class CTCClient : public CTCSocket
{
public:
	CTCClient() : CTCSocket() {}
	~CTCClient() {}
	bool Open(const std::string &address, const std::string &modules, uint16_t port);
	void Receive(std::queue<std::unique_ptr<STCPacket>> &queue, int ms);
	void ReConnect(); // NNG redials on its own; this only paces callers that retry while it does
};
