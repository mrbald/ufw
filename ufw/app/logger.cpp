/*
   Copyright 2017-2026 Vladimir Lysyy (mrbald@github)

   SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
*/

#include "logger.hpp"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/ConsoleSink.h>

#include <memory>
#include <vector>

namespace ufw {

namespace {

quill::LogLevel to_log_level(std::string const& s)
{
    if (s == "trace") { return quill::LogLevel::TraceL3; }
    if (s == "debug") { return quill::LogLevel::Debug; }
    if (s == "info") { return quill::LogLevel::Info; }
    if (s == "warning" || s == "warn") { return quill::LogLevel::Warning; }
    if (s == "error") { return quill::LogLevel::Error; }
    if (s == "critical" || s == "fatal") { return quill::LogLevel::Critical; }
    return quill::LogLevel::Info;
}

logger_config& current_config()
{
    static logger_config cfg;
    return cfg;
}

std::vector<std::shared_ptr<quill::Sink>> default_sinks()
{
    static auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("ufw_console");
    return {sink};
}

quill::PatternFormatterOptions current_pattern()
{
    return quill::PatternFormatterOptions{
        current_config().pattern, current_config().timestamp_pattern};
}

} // anonymous namespace

void initialize_logger()
{
    quill::Backend::start();
}

void configure_logger(logger_config const& cfg)
{
    current_config() = cfg;
    auto const lvl = to_log_level(cfg.severity);
    // Apply the new severity to the root logger and any already-created entity
    // loggers; pattern changes only affect loggers created after this point
    // (Quill's pattern is fixed at logger creation time).
    get_root_logger()->set_log_level(lvl);
}

logger_t get_root_logger()
{
    static logger_t root = quill::Frontend::create_or_get_logger(
        "app", default_sinks(), current_pattern());
    return root;
}

logger_t get_or_create_entity_logger(std::string const& entity_id)
{
    return quill::Frontend::create_or_get_logger(
        entity_id, default_sinks(), current_pattern());
}

} // namespace ufw
