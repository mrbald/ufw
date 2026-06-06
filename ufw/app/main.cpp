/*
   Copyright 2017-2026 Vladimir Lysyy (mrbald@github)

   SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
*/

#include "application.hpp"
#include "exception_types.hpp"
#include "logger.hpp"
#include "library_repository.hpp"
#include "plugin_repository.hpp"

#include <boost/exception/diagnostic_information.hpp>


int main(int argc, char const** argv)
{
    ufw::initialize_logger();

    try
    {
        LOG_INF("starting");
        ufw::application app;
        app.add<ufw::library_repository>("LIBRARY");
        app.add<ufw::plugin_repository>("PLUGIN");

        app.load(argc, argv);

        app.run();
    }
    catch (...)
    {
        LOG_ERR("{}", boost::current_exception_diagnostic_information());
        return 1;
    }
}
