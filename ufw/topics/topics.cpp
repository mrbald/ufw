#include "topics.hpp"

namespace ufw {

topic_payload_id_t next_topic_payload_id() {
    static topic_payload_id_t topic_payload_id_seq {0};
    return ++topic_payload_id_seq;
}

} // namespace ufw

namespace {
    using namespace ufw;

} // local namespace
