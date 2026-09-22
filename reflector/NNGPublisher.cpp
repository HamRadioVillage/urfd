#include "NNGPublisher.h"
#include "Global.h"
#include <ctime>
#include <iostream>
#include <sstream>

CNNGPublisher::CNNGPublisher()
    : m_started(false)
{
    m_sock.id = 0;
}

CNNGPublisher::~CNNGPublisher()
{
    Stop();
}

bool CNNGPublisher::Start(const std::string &addr)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_started) return true;

    int rv;
    if ((rv = nng_pub0_open(&m_sock)) != 0) {
        std::cerr << "NNG: Failed to open pub socket: " << nng_strerror(rv) << std::endl;
        return false;
    }

    if ((rv = nng_listen(m_sock, addr.c_str(), nullptr, 0)) != 0) {
        std::cerr << "NNG: Failed to listen on " << addr << ": " << nng_strerror(rv) << std::endl;
        nng_close(m_sock);
        return false;
    }

    m_started = true;
    std::cout << "NNG: Publisher started at " << addr << std::endl;
    return true;
}

void CNNGPublisher::Stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_started) return;

    nng_close(m_sock);
    m_started = false;
    std::cout << "NNG: Publisher stopped" << std::endl;
}

nlohmann::json CNNGPublisher::NewEvent(const char *type)
{
    nlohmann::json event;
    event["type"] = type;
    event["reflector"] = g_Reflector.GetCallsign().GetCS();

    // %FT%TZ, matching the ConnectTime and LastHeard fields already emitted
    // in the state payload, so a subscriber needs only one date format.
    //
    // gmtime_r, not gmtime: events are raised on protocol threads, several of
    // which can be in here at once, and gmtime returns a pointer into a single
    // shared struct. The other gmtime calls in this tree are all on the
    // maintenance thread, so they are safe as they stand.
    const std::time_t now = std::time(nullptr);
    struct tm utc;
    char s[32];
    if (std::strftime(s, sizeof(s), "%FT%TZ", gmtime_r(&now, &utc)))
        event["timestamp"] = s;

    return event;
}

void CNNGPublisher::Publish(const nlohmann::json &event)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_started) return;

    if (m_sock.id == 0) {
        std::cerr << "NNG debug: Cannot publish, socket not initialized." << std::endl;
        return;
    }
    std::string msg = event.dump();
    if (g_Configure.GetBoolean(g_Keys.dashboard.debug))
        std::cout << "NNG debug: Attempting to publish message of size " << msg.size() << ": " << msg << std::endl;
    int rv = nng_send(m_sock, (void *)msg.c_str(), msg.size(), NNG_FLAG_NONBLOCK);
    if (rv == 0) {
        // Count event instead of logging
        std::string type = event["type"];
        m_EventCounts[type]++;
    } else if (rv != NNG_EAGAIN) {
        std::cerr << "NNG: Send error: " << nng_strerror(rv) << std::endl;
    }
}

std::string CNNGPublisher::GetAndClearStats()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_EventCounts.empty()) return "";

    std::stringstream ss;
    bool first = true;
    for (const auto& kv : m_EventCounts)
    {
        if (!first) ss << ", ";
        ss << "\"" << kv.first << "\": " << kv.second;
        first = false;
    }
    m_EventCounts.clear();
    return ss.str();
}
