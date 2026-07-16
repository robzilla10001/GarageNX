// source/services/service_manager.cpp

#include "services/service_manager.hpp"

namespace Services {

const char* status_str(Status s) {
    switch (s) {
        case Status::Stopped:  return "Stopped";
        case Status::Starting: return "Starting";
        case Status::Running:  return "Running";
        case Status::Error:    return "Error";
    }
    return "?";
}

NetworkService::~NetworkService() {
    stop();
}

bool NetworkService::start() {
    if (is_running()) return false;
    m_stop.store(false);
    set_status(Status::Starting);

#ifdef PLATFORM_SWITCH
    // 256 KiB stack, default priority relative to caller (-2), unpinned core (-2).
    if (R_FAILED(threadCreate(&m_thread, &NetworkService::thread_entry, this,
                              nullptr, 0x40000, 0x2C, -2))) {
        set_status(Status::Error);
        set_error("threadCreate failed");
        return false;
    }
    if (R_FAILED(threadStart(&m_thread))) {
        threadClose(&m_thread);
        set_status(Status::Error);
        set_error("threadStart failed");
        return false;
    }
#else
    m_thread = std::thread(&NetworkService::thread_main, this);
#endif

    m_thread_active = true;
    return true;
}

void NetworkService::stop() {
    if (!m_thread_active) return;
    m_stop.store(true);

#ifdef PLATFORM_SWITCH
    threadWaitForExit(&m_thread);
    threadClose(&m_thread);
#else
    if (m_thread.joinable()) m_thread.join();
#endif

    m_thread_active = false;
    if (status() != Status::Error) set_status(Status::Stopped);
}

#ifdef PLATFORM_SWITCH
void NetworkService::thread_entry(void* self) {
    static_cast<NetworkService*>(self)->thread_main();
}
#endif

void NetworkService::thread_main() {
    set_status(Status::Running);
    run();
    // run() returns either because should_stop() was set, or because it errored
    // (in which case it will have called set_status(Error)).
    if (status() == Status::Running) set_status(Status::Stopped);
}

void NetworkService::set_error(const std::string& e) {
    {
        std::lock_guard<std::mutex> lk(m_error_mtx);
        m_error = e;
    }
    set_status(Status::Error);
}

std::string NetworkService::last_error() const {
    std::lock_guard<std::mutex> lk(m_error_mtx);
    return m_error;
}

} // namespace Services
