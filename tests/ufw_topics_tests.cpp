/* Copyright (c) 2018 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#include <boost/test/unit_test.hpp>

#include <ufw/topics/topics.hpp>

#include <string>
#include <tuple>

namespace {

BOOST_AUTO_TEST_SUITE(ufw_topics)

BOOST_AUTO_TEST_CASE(topic_id_test) {
    using sig_1 = std::tuple<int, long, double>;
    using sig_2 = std::tuple<double, long, int>;

    auto const sub_1 = std::string("hello");
    auto const sub_2 = std::string("olleh");

    using ufw::topic_id_for;

    BOOST_REQUIRE_EQUAL(topic_id_for<sig_1>(sub_1), topic_id_for<sig_1>(sub_1));
    BOOST_REQUIRE_NE(topic_id_for<sig_1>(sub_1), topic_id_for<sig_1>(sub_2));
    BOOST_REQUIRE_NE(topic_id_for<sig_1>(sub_1), topic_id_for<sig_2>(sub_1));
} // BOOST_AUTO_TEST_CASE(topic_id_test)

BOOST_AUTO_TEST_SUITE_END(/* ufw_topics */)

} // local namespace
