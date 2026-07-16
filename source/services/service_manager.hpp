#pragma once
// source/services/service_manager.hpp
// Base class + thread lifecycle for the M6 background services (FTP/HTTP/MTP).
// Concrete services implement run(), which must loop until should_stop() is true
// and check it often enough (e.g. via a select() timeout) that stop() returns
// promptly. The thread primitive is libnx Thread on Switch / std::thread on PC,
// hidden entirely behind this base.

#include <atomic>
#include <mutex>
#include <string>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#else
#include <thread>
#endif

namespace Services {

enum class Status { Stopped, Starting, Running, Error };

const char* status_str(Status s);

class NetworkService {
public:
    virtual ~NetworkService();

    // Spawn the worker thread. Returns false if already running or on failure.
    bool start();
    // Signal the worker to stop and join it. Safe to call when already stopped.
    void stop();

    bool   is_running() const {
        Status s = m_status.load();
        return s == Status::Running || s == Status::Starting;
    }
    Status status()     const { return m_status.load(); }
    std::string last_error() const;

    virtual const char* name() const = 0;

protected:
    // Concrete service loop. Runs on the worker thread; return when should_stop().
    virtual void run() = 0;

    bool should_stop() const { return m_stop.load(); }
    void set_status(Status s) { m_status.store(s); }
    void set_error(const std::string& e);

private:
    void thread_main();
#ifdef PLATFORM_SWITCH
    static void thread_entry(void* self);
    Thread      m_thread{};
#else
    std::thread m_thread;
#endif
    std::atomic<Status> m_status{Status::Stopped};
    std::atomic<bool>   m_stop{false};
    bool                m_thread_active = false;
    mutable std::mutex  m_error_mtx;
    std::string         m_error;
};

} // namespace Services
