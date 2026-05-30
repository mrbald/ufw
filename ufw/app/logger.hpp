/*
   Copyright 2017-2026 Vladimir Lysyy (mrbald@github)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#pragma once

#include "configuration.hpp"

#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>

#include <string>

namespace ufw {

using logger_t = quill::Logger*;

struct logger_config
{
    std::string severity {"info"};
    std::string pattern {
        "%(time) | %(log_level:<7) | %(thread_id) | %(logger) - %(message)"};
    std::string timestamp_pattern {"%H:%M:%S.%Qms"};
};

void initialize_logger();
void configure_logger(logger_config const& cfg);

logger_t get_root_logger();
logger_t get_or_create_entity_logger(std::string const& entity_id);

} // namespace ufw

// Macros expand at the call site, where unqualified `get_logger()` resolves
// either to an entity's member (via ENTITY_LOGGER) or to the free function
// below (root logger fallback for non-entity contexts).
#define LOG_DBG(...) QUILL_LOG_DEBUG  (get_logger(), __VA_ARGS__)
#define LOG_INF(...) QUILL_LOG_INFO   (get_logger(), __VA_ARGS__)
#define LOG_WRN(...) QUILL_LOG_WARNING(get_logger(), __VA_ARGS__)
#define LOG_ERR(...) QUILL_LOG_ERROR  (get_logger(), __VA_ARGS__)

inline ::ufw::logger_t get_logger() { return ::ufw::get_root_logger(); }

namespace YAML {
template <>
struct convert<ufw::logger_config>
{
    static Node encode(ufw::logger_config const& rhs)
    {
        Node node;
        CFG_ENCODE(severity);
        CFG_ENCODE(pattern);
        CFG_ENCODE(timestamp_pattern);
        return node;
    }

    static bool decode(Node const& node, ufw::logger_config& rhs)
    {
        if (!node.IsMap())
        {
            return false;
        }
        CFG_DECODE_IF_SET(severity);
        CFG_DECODE_IF_SET(pattern);
        CFG_DECODE_IF_SET(timestamp_pattern);
        return true;
    }
};
} // namespace YAML
