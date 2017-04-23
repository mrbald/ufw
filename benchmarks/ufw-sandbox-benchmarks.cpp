#include <benchmark/benchmark.h>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>

#include <capnp/compat/json.h>

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


void ufw_capnp_json_reader_benchmark(benchmark::State& state) {
    std::string const input {R"({"id": "ssp1-27009141-1485848975137","at": 2,"imp": [{"id": "1","bidfloor": 13.521766169371157,"bidfloorcur": "RUB","banner": {"pos": 0,"h": 100,"w": 300},"secure": 0}],"site": {"id": "10930","domain": "warfiles.ru","ref": "http://warfiles.ru/show-142725-rossiya-vpervye-ispytala-noveyshuyu-aviabombu-drel-v-sirii.html","publisher": {"id": "9886"},"ext": {"place_id": 79170},"cat": ["IAB12"]},"device": {"ua": "Mozilla/5.0 (iPad; CPU OS 9_3_5 like Mac OS X) AppleWebKit/601.1.46 (KHTML, like Gecko) YaBrowser/16.11.0.2708.11 Mobile/13G36 Safari/601.1","ip": "84.234.53.206","make": "Apple","model": "iPad","os": "iOS","osv": "9.0","devicetype": 5},"user": {"id": "35e4f8a5-e897-4589-a075-bc2c8acd7e1e","buyeruid": "6331632281203562848","geo": {"type": 2,"city": "Moscow","region": "MD-BD","country": "Russia"}}})"};

    capnp::JsonCodec json;

    int fd = open("/dev/null", O_WRONLY|O_CREAT|O_APPEND);
    while (state.KeepRunning())
    {
        capnp::MallocMessageBuilder message{ kj::Array<capnp::word>{buffer.data(), buffer.size(), kj::NullArrayDisposer{}}};

        auto jsonValue = message.initRoot<capnp::JsonValue>();
        json.decodeRaw(kj::Array<const char>{input.data(), input.size(), kj::NullArrayDisposer{}}, jsonValue);
    }
    close(fd);
}

BENCHMARK(ufw_capnp_json_reader_benchmark);

} // local namespace
