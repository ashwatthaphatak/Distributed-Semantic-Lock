#include "threadsafe_log.h"

#include <iostream>
#include <mutex>

namespace {
std::mutex g_log_mu;
}

void log_line(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_log_mu);
    std::cout << line << std::endl;
}
