/*
   Copyright 2017 Vladimir Lysyy (mrbald@github)

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

    std::shared_ptr<message const> msg() const { return msg_; }

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
