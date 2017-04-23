@0x9b37c3bc7faba92f; # these are generated with 'capnp id'

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("ufw::sandbox");

struct Geo @0xa0cb52bd98362053 {
    id @0 :UInt64;
    city @1 :Text;
    country @2 :Text;
}

struct Ad @0xb5e8f170e6d0d546 {
    id @0 :UInt64;
    width @1 :Int16;
    height @2 :Int16;
    position @3 :Int16;
    maxBidMicros @4 :UInt64;
    code @5 :Text;
    record @6 :Text;
}

struct GeoAd @0x9d4eb6153653e461 {
    geo @0 :Geo;
    ad @1 :Ad;
}

struct GeoAds @0xba663e5b64558444 {
    geo @0 :Geo;
    ads @1 :List(Ad);
}

struct GeoAdsLite @0xfa910c5864f1e1f2 {
    geoId @0 :UInt64;
    adIds @1 :List(UInt64);
}

interface GeoCache @0x841946f3e96607b5 {
    get @0 (id :UInt64) -> (geo :Geo);
    getAll @1 (city :Text, country :Text) -> (geos :List(Geo));
    find @2 (city :Text, country :Text) -> (geo :Geo);
}

