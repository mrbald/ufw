#include "logger.hpp"

#include <boost/log/expressions.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/from_stream.hpp>
#include <boost/log/utility/setup/formatter_parser.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <string>

namespace logging  = boost::log;
namespace attrs    = boost::log::attributes;
namespace expr     = boost::log::expressions;
namespace src      = boost::log::sources;
namespace keywords = boost::log::keywords;

namespace ufw {

struct TimeStampFormatterFactory: boost::log::basic_formatter_factory<char, boost::posix_time::ptime>
{
    formatter_type create_formatter(const boost::log::attribute_name& name, const args_map& args)
    {
        auto it = args.find("format");
        if (it != args.end()) {
            return boost::log::expressions::stream
                << boost::log::expressions::format_date_time<boost::posix_time::ptime>(
                    boost::log::expressions::attr<boost::posix_time::ptime>(name), it->second);
        }
        else
        {
            return boost::log::expressions::stream
                << boost::log::expressions::attr<boost::posix_time::ptime>(name);
        }
    }
};

void initialize_logger()
{
    LOG_STAMP_THREAD;
    logging::register_formatter_factory("TimeStamp", boost::make_shared<TimeStampFormatterFactory>());
    logging::register_simple_formatter_factory<logging::trivial::severity_level, char>("Severity");
    logging::core::get()->add_global_attribute("File", attrs::mutable_constant<logging::string_literal>("-"));
    logging::core::get()->add_global_attribute("Line", attrs::mutable_constant<int>(0));
    logging::core::get()->add_global_attribute("Func", attrs::mutable_constant<logging::string_literal>("-"));
    logging::core::get()->add_global_attribute("Tag", attrs::mutable_constant<logging::string_literal>("-"));
    logging::add_common_attributes();
}

void configure_logger(std::string const& cfg)
{
    std::istringstream buf(cfg);
    boost::log::init_from_stream(buf);
}

} // namespace ufw
