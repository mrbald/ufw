/*
   Copyright 2017-2026 Vladimir Lysyy (mrbald@github)

   SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
*/

#pragma once

#include <stdexcept>
#include <memory>

namespace ufw {

struct message;
using resolved_entity_id = size_t;

struct fatal_error: std::runtime_error { using std::runtime_error::runtime_error; };
struct transient_error: std::runtime_error { using std::runtime_error::runtime_error; };

struct bad_entity_reference: fatal_error { using fatal_error::fatal_error; };


struct messaging_error: transient_error
{
    template <class... Args>
    messaging_error(std::shared_ptr<message const> msg, resolved_entity_id src, Args&&... args):
            transient_error(std::forward<Args>(args)...),
            msg_{std::move(msg)},
            src_{src} {}

    [[nodiscard]] std::shared_ptr<message const> msg() const { return msg_; }
    [[nodiscard]] resolved_entity_id origin() const { return src_; }
private:
    std::shared_ptr<message const> const msg_;
    resolved_entity_id const src_;
};


struct destination_unreachable_error: messaging_error
{
    destination_unreachable_error(std::shared_ptr<message const> msg, resolved_entity_id src):
            messaging_error(std::move(msg), src, "destination unreachable") {}
};


struct routing_error: messaging_error { using messaging_error::messaging_error; };

struct route_not_found_error: routing_error
{
    route_not_found_error(std::shared_ptr<message const> msg, resolved_entity_id src):
            routing_error(std::move(msg), src, "route not found message") {}
};

} // namespace ufw
