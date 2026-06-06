/*
   Copyright 2017-2026 Vladimir Lysyy (mrbald@github)

   SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
*/

#define BOOST_TEST_MODULE "ufw"
// Header-only Boost.Test: the 'included' variant compiles the framework into this
// one TU, so we link no compiled boost component. Other test TUs include the plain
// <boost/test/unit_test.hpp> and resolve against the symbols defined here.
#include <boost/test/included/unit_test.hpp>
