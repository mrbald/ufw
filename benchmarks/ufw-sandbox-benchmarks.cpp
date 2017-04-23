#include <benchmark/benchmark.h>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>

#include <sandbox/sandbox.capnp.h>

#include <string>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace {

std::array<capnp::word, 16384> buffer;

void ufw_capnp_benchmark(benchmark::State& state) {
    using namespace ufw::sandbox;

    int fd = open("/dev/null", O_WRONLY|O_CREAT|O_APPEND);
    while (state.KeepRunning())
    {
        capnp::MallocMessageBuilder message{ kj::Array<capnp::word>{buffer.data(), buffer.size(), kj::NullArrayDisposer{}}};

        // auto geoAd = message.initRoot<GeoAd>();
        auto geoAds = message.initRoot<GeoAds>();

        //auto geo = geoAd.initGeo();
        auto geo = geoAds.initGeo();
        geo.setId(3);
        geo.setCity("Nobosibirsk");
        geo.setCountry("Russia");

        auto ads = geoAds.initAds(64);
        for (int i = 0; i < 64; ++i) {
            auto ad = ads[i];

            ad.setId(7 + i);
            ad.setWidth(320);
            ad.setHeight(200);
            ad.setPosition(1 + i);
            ad.setMaxBidMicros(100000);
            ad.setCode(R"(<script language="JavaScript"></script>)");
        }

        // writePackedMessageToFd(fd, message);
        writeMessageToFd(fd, message);
    }
    close(fd);
}

BENCHMARK(ufw_capnp_benchmark);

void ufw_capnp_lite_benchmark(benchmark::State& state) {
    using namespace ufw::sandbox;

    int fd = open("/dev/null", O_WRONLY|O_CREAT|O_APPEND);
    while (state.KeepRunning())
    {
        capnp::MallocMessageBuilder message{ kj::Array<capnp::word>{buffer.data(), buffer.size(), kj::NullArrayDisposer{}}};

        auto geoAds = message.initRoot<GeoAdsLite>();
        geoAds.setGeoId(42);

        auto ads = geoAds.initAdIds(64);
        for (int i = 0; i < 64; ++i) {
            ads.set(i, 42 + i);
        }

        // writePackedMessageToFd(fd, message);
        writeMessageToFd(fd, message);
    }
    close(fd);
}

BENCHMARK(ufw_capnp_lite_benchmark);


} // local namespace
