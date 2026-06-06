/*
 * Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * The boost::asio io_context as a worker's backstop (ufw/core/exec/poll_source.hpp):
 *   - poll()          non-blocking drain of ready handlers — a SPINNING worker calls
 *                     this each turn so signals/timers/posts stay serviced;
 *   - poll_blocking() run_one(): the BLOCKING worker's park — returns on IO, on a
 *                     posted handler, or on wake();
 *   - wake()          post a no-op handler to unpark run_one() (this is what lets a
 *                     parked blocking worker be a dispatch matrix target, and what
 *                     makes request_stop() reach a parked loop).
 *
 * Lives in ufw_app (ufw_core must stay boost-asio-free). Handlers run through the
 * worker loops execute inside noexcept poll calls: a throwing handler terminates —
 * deliberate fail-fast, same end state as an uncaught exception from context_.run().
 */
#pragma once

#include <ufw/core/exec/poll_source.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <cstddef>

namespace ufw {

class asio_backstop final : public core::blocking_source
{
public:
    explicit asio_backstop(boost::asio::io_context& ctx) noexcept: ctx_{&ctx} {}

    std::size_t poll() noexcept override { return ctx_->poll(); }
    void poll_blocking() noexcept override { ctx_->run_one(); }
    void wake() noexcept override { boost::asio::post(*ctx_, [] {}); }

private:
    boost::asio::io_context* ctx_;
};

} // namespace ufw
