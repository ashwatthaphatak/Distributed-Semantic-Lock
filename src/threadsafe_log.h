// Author: Ayush Gala
// Declares the thread-safe line logger used across the repo.
// This keeps multi-threaded demo and server output readable.

#pragma once

#include <string>

// Serializes a log line to stdout under a mutex, preventing interleaved output across threads.
void log_line(const std::string& line);
