#include "entity.hpp"
#include "lifecycle_participant.hpp"

namespace {

struct example: ufw::entity, ufw::lifecycle_participant {
    using ufw::entity::entity;

    void init() override /* from lifecycle_participant */
    {
        LOG_INF << "initialized";
    }

    void start() override /* from lifecycle_participant */
    {
        LOG_INF << "started";
    }
};

extern "C" ufw::entity* example_ctor(ufw::entity_id const& id, ufw::resolved_entity_id rid, ufw::application& app) {
    return new example {id, rid, app};
}

} // local namespace
