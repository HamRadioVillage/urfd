#pragma once

#include <string>
#include <mutex>
#include <nlohmann/json.hpp>
#include <map>
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>

class CNNGPublisher
{
public:
    CNNGPublisher();
    ~CNNGPublisher();

    bool Start(const std::string &addr);
    void Stop();

    // Builds an event pre-stamped with its type, the emitting reflector's
    // callsign and the current UTC time. Stamping happens here, at the moment
    // the event occurs, rather than inside Publish(): an event may be queued
    // before it reaches the socket, and the time it was observed is not the
    // time it was sent.
    static nlohmann::json NewEvent(const char *type);

    void Publish(const nlohmann::json &event);

    std::string GetAndClearStats();

private:
    nng_socket m_sock;
    std::mutex m_mutex;
    bool m_started;
    
    // Event counters
    std::map<std::string, int> m_EventCounts;
};
