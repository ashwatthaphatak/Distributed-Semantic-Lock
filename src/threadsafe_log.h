// Declares the thread-safe line logger used across the repo.
// This keeps multi-threaded demo and server output readable.

#pragma once

#include <string>

void log_line(const std::string& line);
