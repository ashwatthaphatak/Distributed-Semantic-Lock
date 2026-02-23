#pragma once

#include <cstdlib>
#include <string>

namespace dscc {

struct NodeConfig {
    std::string node_id = std::getenv("NODE_ID") ? std::getenv("NODE_ID") : "1";
    std::string port = std::getenv("PORT") ? std::getenv("PORT") : "5001";
};

} // namespace dscc
