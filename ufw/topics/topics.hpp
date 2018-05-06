/* Copyright (c) 2018 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#pragma once

#include <ufw/app/exception_types.hpp>

#include <boost/core/demangle.hpp>

#include <typeinfo>
#include <limits>
#include <atomic>
#include <string>
#include <stdexcept>
#include <map>

namespace ufw {

using topic_payload_id_t = uint32_t;
using topic_subject_id_t = uint32_t;
using topic_id_t = uint64_t;

extern topic_payload_id_t next_topic_payload_id();

template <typename T> inline
topic_payload_id_t verified_topic_payload_id(topic_payload_id_t const topic_payload_id) {
    if (topic_payload_id == std::numeric_limits<topic_payload_id_t>::max()) {
        throw ufw::fatal_error("cound not register payload type ["
                + boost::core::demangle(typeid(T).name())
                +"] - too many payload types ("
                + std::to_string(topic_payload_id)
                + ") already registered");
    }

    return topic_payload_id;
}

using topic_subject_interner_t = std::map<std::string, topic_subject_id_t>;

template <typename T> inline
topic_subject_id_t verified_topic_subject_id(std::string const& subject, topic_subject_interner_t& interner) {
    auto const next_topic_subject_id = interner.size() + 1;
    auto const status = interner.try_emplace(subject, next_topic_subject_id);
    if (status.second && next_topic_subject_id == std::numeric_limits<topic_subject_id_t>::max()) {
        throw ufw::fatal_error("cound not register subject ["
                + subject
                + "] for payload type ["
                + boost::core::demangle(typeid(T).name())
                +"] - maximum number topic subjects ("
                + std::to_string(next_topic_subject_id)
                + ") already registered for the payload type");
    }

    return status.first->second;
}

template <typename T> inline
topic_id_t topic_id_for(std::string const& subject) {
    static topic_payload_id_t const topic_payload_id
            = verified_topic_payload_id<T>(next_topic_payload_id());

    static std::map<std::string, topic_subject_id_t> interner;

    auto const topic_subject_id = verified_topic_subject_id<T>(subject, interner);

    return (topic_payload_id << sizeof(topic_payload_id)) + topic_subject_id;
}

} // namespace ufw
