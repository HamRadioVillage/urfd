#include <iostream>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <cstring>
#include <sstream>
#include <arpa/inet.h>

#include "TCSocket.h"

// This file is shared with tcd, which links to it, so both ends of the link run this code.
//
// Each end pings the other every PING_INTERVAL_MS. A peer that has been heard pinging but then
// goes quiet for PEER_TIMEOUT_MS is assumed gone and its connection is closed. Without this an idle
// TCP connection can be dropped by a NAT or firewall without either end noticing, and pair1 then
// refuses the transcoder's reconnects because it still believes it has a peer.
//
// Pings carry module '\0', which no end has configured, so a peer from before the keepalive drops them.
static constexpr int64_t PING_INTERVAL_MS = 1000;
static constexpr int64_t PEER_TIMEOUT_MS = 5000;

static int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::string PipeAddress(nng_pipe pipe)
{
	nng_sockaddr sa;
	if (nng_pipe_get_addr(pipe, NNG_OPT_REMADDR, &sa) != 0)
		return "unknown address";

	char buf[INET6_ADDRSTRLEN] = "";
	switch (sa.s_family)
	{
	case NNG_AF_INET:
		inet_ntop(AF_INET, &sa.s_in.sa_addr, buf, sizeof(buf));
		return std::string(buf) + ":" + std::to_string(ntohs(sa.s_in.sa_port));
	case NNG_AF_INET6:
		inet_ntop(AF_INET6, sa.s_in6.sa_addr, buf, sizeof(buf));
		return "[" + std::string(buf) + "]:" + std::to_string(ntohs(sa.s_in6.sa_port));
	case NNG_AF_IPC:
		return sa.s_ipc.sa_path;
	default:
		return "unknown address";
	}
}

CTCSocket::CTCSocket() : m_Running(false), m_Connected(false), m_LastRxMs(0), m_DownSinceMs(NowMs()), m_PeerPings(false)
{
	m_Sock.id = 0;
	m_Pipe.id = 0;
}

CTCSocket::~CTCSocket()
{
	Close();
}

void CTCSocket::Close()
{
	m_Running = false;
	if (m_Thread.joinable())
		m_Thread.join();

	if (m_Sock.id != 0)
	{
		nng_close(m_Sock);
		m_Sock.id = 0;
	}
	m_Connected = false;
}

void CTCSocket::Close(char module)
{
	// In multiplexed mode, we cannot close a single module's connection independently
	// without closing the whole pipe. So this is a no-op or full close.
	// For now, no-op to allow other modules to survive transient errors.
	// std::cerr << "Close(" << module << ") ignored in NNG mode" << std::endl;
}

bool CTCSocket::Send(const STCPacket *packet)
{
	if (m_Sock.id == 0) return true;

	// pair1 would hold this for whichever peer connects next, by which time it is stale audio
	if (! m_Connected) return true;

	int rv = nng_send(m_Sock, (void*)packet, sizeof(STCPacket), 0);
	if (rv != 0)
	{
		// std::cerr << "NNG Send Error: " << nng_strerror(rv) << std::endl;
		return true;
	}
	return false;
}

bool CTCSocket::IsConnected(char module) const
{
	return m_Connected;
}

int CTCSocket::GetFD(char module) const
{
	// Legacy helper for checking connection state
	// CodecStream expects < 0 on failure
	return m_Connected ? 1 : -1;
}

unsigned CTCSocket::SecondsDisconnected() const
{
	if (m_Connected) return 0;
	return unsigned((NowMs() - m_DownSinceMs) / 1000);
}

bool CTCSocket::OpenSocket()
{
	int rv;
	if ((rv = nng_pair1_open(&m_Sock)) != 0)
	{
		std::cerr << "nng_pair1_open failed: " << nng_strerror(rv) << std::endl;
		return true;
	}

	// Set receive timeout to 100ms for dispatcher loop
	nng_socket_set_ms(m_Sock, NNG_OPT_RECVTIMEO, 100);
	// A link that has stalled but not yet timed out must not hang the sender
	nng_socket_set_ms(m_Sock, NNG_OPT_SENDTIMEO, 100);

	for (auto ev : { NNG_PIPE_EV_ADD_PRE, NNG_PIPE_EV_ADD_POST, NNG_PIPE_EV_REM_POST })
	{
		if ((rv = nng_pipe_notify(m_Sock, ev, PipeEvent, this)) != 0)
		{
			std::cerr << "nng_pipe_notify failed: " << nng_strerror(rv) << std::endl;
			return true;
		}
	}
	return false;
}

void CTCSocket::PipeEvent(nng_pipe pipe, nng_pipe_ev ev, void *arg)
{
	auto self = static_cast<CTCSocket *>(arg);
	const auto now = NowMs();
	nng_pipe toClose;
	toClose.id = 0;
	{
		std::lock_guard<std::mutex> lock(self->m_PipeMutex);
		switch (ev)
		{
		case NNG_PIPE_EV_ADD_PRE:
			// pair1 will turn this one away because it already has a peer. If that peer has gone
			// quiet it is most likely a dead connection this one is replacing, so close it, and the
			// newcomer's next attempt gets in. A peer that predates the keepalive is quiet whenever
			// nobody is talking, but it only redials after losing its connection, so the same holds.
			if (nng_pipe_id(self->m_Pipe) > 0)
			{
				const auto addr = PipeAddress(pipe);
				const auto silent = now - self->m_LastRxMs;
				if (silent > PEER_TIMEOUT_MS)
				{
					std::cout << "TC link: connection from " << addr << " while " << self->m_PeerAddr << " has been silent for " << silent / 1000 << "s, closing the old connection" << std::endl;
					toClose = self->m_Pipe;
				}
				else
					std::cerr << "TC link: refusing connection from " << addr << ", already connected to " << self->m_PeerAddr << std::endl;
			}
			break;
		case NNG_PIPE_EV_ADD_POST:
			self->m_Pipe = pipe;
			self->m_PeerAddr = PipeAddress(pipe);
			self->m_LastRxMs = now;
			self->m_PeerPings = false;
			self->m_Connected = true;
			std::cout << "TC link UP: connected to " << self->m_PeerAddr << " after " << (now - self->m_DownSinceMs) / 1000 << "s down" << std::endl;
			break;
		case NNG_PIPE_EV_REM_POST:
			// Pipes pair1 turned away come through here too, so only act on the one we have
			if (nng_pipe_id(pipe) == nng_pipe_id(self->m_Pipe))
			{
				self->m_Pipe.id = 0;
				self->m_Connected = false;
				self->m_DownSinceMs = now;
				std::cout << "TC link DOWN: lost connection to " << self->m_PeerAddr << std::endl;
			}
			break;
		default:
			break;
		}
	}
	if (nng_pipe_id(toClose) > 0)
		nng_pipe_close(toClose);
}

void CTCSocket::SendPing()
{
	if (! m_Connected) return;

	STCPacket ping{};
	ping.codec_in = ECodecType::ping;
	// The dispatcher sends these, and must not wait on a backed-up link to do it
	nng_send(m_Sock, &ping, sizeof(ping), NNG_FLAG_NONBLOCK);
}

void CTCSocket::CheckLiveness(int64_t now)
{
	nng_pipe pipe;
	std::string addr;
	{
		std::lock_guard<std::mutex> lock(m_PipeMutex);
		pipe = m_Pipe;
		addr = m_PeerAddr;
	}
	if (nng_pipe_id(pipe) <= 0 || ! m_PeerPings)
		return;

	const auto silent = now - m_LastRxMs;
	if (silent > PEER_TIMEOUT_MS)
	{
		std::cout << "TC link: nothing from " << addr << " for " << silent / 1000 << "s, closing the connection so it can be re-established" << std::endl;
		m_PeerPings = false; // so this is only said once while the close goes through
		nng_pipe_close(pipe);
	}
}

void CTCSocket::Dispatcher()
{
	int64_t lastPing = 0;
	while (m_Running)
	{
		const auto now = NowMs();
		if (now - lastPing >= PING_INTERVAL_MS)
		{
			lastPing = now;
			SendPing();
			CheckLiveness(now);
		}

		STCPacket *buf = nullptr;
		size_t sz = 0;
		// 100ms timeout to check m_Running
		int rv = nng_recv(m_Sock, &buf, &sz, NNG_FLAG_ALLOC);

		if (rv == 0)
		{
			m_LastRxMs = NowMs();
			if (sz == sizeof(STCPacket))
			{
				STCPacket pkt;
				memcpy(&pkt, buf, sizeof(STCPacket));
				nng_free(buf, sz);

				if (pkt.codec_in == ECodecType::ping)
				{
					m_PeerPings = true;
					continue;
				}

				// Log first packet from this module
				if (m_SeenModules.find(pkt.module) == m_SeenModules.end())
				{
					std::cout << "NNG: Received first packet from module " << pkt.module << std::endl;
					m_SeenModules.insert(pkt.module);
				}

                {
                    std::lock_guard<std::mutex> lock(m_StatsMutex);
                    m_PacketCounts[pkt.module]++;
                }

				if (m_ClientQueue)
				{
					// Client mode: everything goes to one queue
					m_ClientQueue->Push(pkt);
				}
				else
				{
					// Server mode: route by module
					auto it = m_Queues.find(pkt.module);
					if (it != m_Queues.end())
					{
						it->second->Push(pkt);
					}
					else
					{
						// Unknown module or not configured?
						// In urfd, we might want to auto-create logic or drop?
						// For now drop, as configured modules are set in Open
					}
				}
			}
			else
			{
				nng_free(buf, sz);
				std::cerr << "Received packet of incorrect size: " << sz << std::endl;
			}
		}
		else if (rv != NNG_ETIMEDOUT)
		{
			// Fatal error?
			// std::cerr << "NNG Recv Error: " << nng_strerror(rv) << std::endl;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}
}

// ---------------- SERVER ----------------

// ---------------- SERVER ----------------

bool CTCServer::Open(const std::string &address, const std::string &modules, uint16_t port)
{
	m_Modules = modules;
	// Initialize queues for configured modules
	for (char c : m_Modules)
	{
		m_Queues[c] = std::make_shared<CTCPacketQueue>();
	}

	int rv;
	if (OpenSocket())
		return true;

	// Increase buffers to prevent blocking/drops during high load/jitter
	int bufSize = 4096;
	nng_socket_set_int(m_Sock, NNG_OPT_RECVBUF, bufSize);
	nng_socket_set_int(m_Sock, NNG_OPT_SENDBUF, bufSize);

	std::stringstream url;
	if (address.find("ipc://") == 0) {
		url << address;
	} else if (address.find("/") == 0 || address.find("./") == 0 || address.find("../") == 0) {
		url << "ipc://" << address;
	} else {
		url << "tcp://" << address << ":" << port;
	}

	if ((rv = nng_listen(m_Sock, url.str().c_str(), nullptr, 0)) != 0)
	{
		std::cerr << "nng_listen failed: " << nng_strerror(rv) << " URL: " << url.str() << std::endl;
		return true;
	}

	m_Running = true;
	m_Thread = std::thread([this] { Dispatcher(); });

	return false;
}

bool CTCServer::Receive(char module, STCPacket *packet, int ms)
{
	auto it = m_Queues.find(module);
	if (it == m_Queues.end()) return false;

	return it->second->Pop(*packet, ms);
}

bool CTCServer::AnyAreClosed() const
{
	// If the dispatcher is running, we assume open.
	// NNG handles reconnections.
	return !m_Running;
}

bool CTCServer::Accept()
{
	// No manual accept needed with NNG
	return false;
}


// ---------------- CLIENT ----------------

bool CTCClient::Open(const std::string &address, const std::string &modules, uint16_t port)
{
	m_Modules = modules;
	m_ClientQueue = std::make_shared<CTCPacketQueue>();

	int rv;
	if (OpenSocket())
		return true;

	std::stringstream url;
	if (address.find("ipc://") == 0) {
		url << address;
	} else if (address.find("/") == 0 || address.find("./") == 0 || address.find("../") == 0) {
		url << "ipc://" << address;
	} else {
		url << "tcp://" << address << ":" << port;
	}

	// Client dials asynchronously so it can retry in background
	if ((rv = nng_dial(m_Sock, url.str().c_str(), nullptr, NNG_FLAG_NONBLOCK)) != 0)
	{
		std::cerr << "nng_dial failed: " << nng_strerror(rv) << " URL: " << url.str() << std::endl;
		return true;
	}

	m_Running = true;
	m_Thread = std::thread([this] { Dispatcher(); });

    // Give it a moment to connect? Not strictly necessary.

	return false;
}

void CTCClient::Receive(std::queue<std::unique_ptr<STCPacket>> &queue, int ms)
{
    // Wait up to ms for the first packet
    STCPacket p;
    if (m_ClientQueue->Pop(p, ms))
    {
        queue.push(std::make_unique<STCPacket>(p));
        // Drain the rest without waiting
        while (m_ClientQueue->Pop(p, 0))
        {
             queue.push(std::make_unique<STCPacket>(p));
        }
    }
}

void CTCClient::ReConnect()
{
	// tcd calls this in a loop until Send() succeeds, which it now cannot do while there is no
	// peer, so wait here rather than spin
	if (! m_Connected)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

std::string CTCSocket::GetAndClearStats()
{
    std::lock_guard<std::mutex> lock(m_StatsMutex);
    if (m_PacketCounts.empty()) return "";

    std::stringstream ss;
    bool first = true;
    for (const auto& kv : m_PacketCounts)
    {
        if (!first) ss << ", ";
        ss << kv.first << ": " << kv.second;
        first = false;
    }
    m_PacketCounts.clear();
    return ss.str();
}
