/*
   Copyright 2017-2023 Vladimir Lysyy (mrbald@github)

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

#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/attributes.hpp>
#include <boost/log/attributes/scoped_attribute.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/thread/thread.hpp>

namespace logging  = boost::log;
namespace attrs    = boost::log::attributes;

template<typename ValueType>
auto set_get_attrib(const char* name, ValueType value) {
   auto attr = logging::attribute_cast<attrs::mutable_constant<ValueType>>(logging::core::get()->get_global_attributes()[name]);
   attr.set(value);
   return attr.get();
}

namespace ufw {

using logger_t = boost::log::sources::severity_logger<logging::trivial::severity_level>;

#define SET_LOG_LEVEL(LVL) do {\
    logging::core::get()->set_filter(logging::trivial::severity >= logging::trivial::LVL); } while (0)

#define LOG_SEV(sev) \
   BOOST_LOG_STREAM_WITH_PARAMS( \
      (get_logger()), \
         (set_get_attrib("File", logging::string_literal(__FILE__))) \
         (set_get_attrib("Line", __LINE__)) \
         (set_get_attrib("Func", logging::string_literal(__func__))) \
         (set_get_attrib("Tag", logging::string_literal(""))) \
         (logging::keywords::severity = (logging::trivial::sev)) \
   ) << ""

#define LOG_SEV_TAGGED(sev, tag) \
   BOOST_LOG_STREAM_WITH_PARAMS( \
      (get_logger()), \
         (set_get_attrib("File", logging::string_literal(__FILE__))) \
         (set_get_attrib("Line", __LINE__)) \
         (set_get_attrib("Func", logging::string_literal(__func__))) \
         (set_get_attrib("Tag", logging::string_literal(tag))) \
         (logging::keywords::severity = (logging::trivial::sev)) \
   ) << ""

#define LOG_DBG LOG_SEV(debug)
#define LOG_INF LOG_SEV(info)
#define LOG_WRN LOG_SEV(warning)
#define LOG_ERR LOG_SEV(error)

uint64_t get_tid() noexcept;

#define LOG_STAMP_THREAD\
    BOOST_LOG_SCOPED_THREAD_ATTR("ThreadPID", attrs::constant<uint64_t>(ufw::get_tid()))


void initialize_logger();
void configure_logger(std::string const& cfg);

} // namespace ufw

inline decltype(auto) get_logger() { return logging::trivial::logger::get(); }
