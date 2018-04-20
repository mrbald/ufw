#include "entity.hpp"
#include "lifecycle_participant.hpp"
#include "application.hpp"

#include <boost/asio/steady_timer.hpp>

namespace {

struct example: ufw::entity, ufw::lifecycle_participant {
    template<class... Args>
    example(Args&&... args):
        entity { std::forward<Args>(args)... },
        timer_ { app().context() }
    {
    }

    void init() override /* from lifecycle_participant */
    {
        LOG_INF << "initialized";
    }

    void start() override /* from lifecycle_participant */
    {
        LOG_INF << "started";
        app().context().post([this]{
            LOG_INF << "scheduling shutdown in 5 seconds";
            timer_.expires_after(std::chrono::seconds(5));
            timer_.async_wait([this](auto&&){
                LOG_INF << ">>>>>> shutting down <<<<<<";
                app().shutdown();
            });
        });
    }

    void stop() noexcept override /* from lifecycle_participant */
    {
        LOG_INF << "stopped";
    }

    void fini() noexcept override /* from lifecycle_participant */
    {
        LOG_INF << "finalized";
    }

private:
    boost::asio::steady_timer timer_;
};

extern "C" ufw::entity* example_ctor(ufw::entity_id const& id, ufw::resolved_entity_id rid, ufw::application& app) {
    return new example {id, rid, app};
}

} // local namespace
