/*
 * Copyright (c) 2015-2026 Vladimir Lysyy (mrbald@github)
 * SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
 *
 * uFW Python control plane (nanobind). Python assembles and drives a C++
 * application — the "mostly A" shape: Python replaces the YAML launcher. The
 * spec dict decodes through the same convert<application_config> path the YAML
 * launcher uses, so plugins (and the slice-2 ABI gate) behave identically.
 */
#include <ufw/app/application.hpp>
#include <ufw/app/configuration.hpp>
#include <ufw/app/library_repository.hpp>
#include <ufw/app/plugin_repository.hpp>
#include <ufw/app/logger.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <yaml-cpp/yaml.h>

#include <stdexcept>
#include <string>

namespace nb = nanobind;
using namespace ufw;

namespace {

// Recursively convert a Python value (dict/list/scalars) into a YAML::Node, so a
// Python spec decodes through exactly the convert<> path the YAML launcher uses.
// bool is checked before int because in Python bool is a subclass of int.
YAML::Node to_yaml(nb::handle obj)
{
    if (obj.is_none())
    {
        return YAML::Node(YAML::NodeType::Null);
    }
    if (nb::isinstance<nb::bool_>(obj))
    {
        return YAML::Node(nb::cast<bool>(obj));
    }
    if (nb::isinstance<nb::int_>(obj))
    {
        return YAML::Node(nb::cast<long long>(obj));
    }
    if (nb::isinstance<nb::float_>(obj))
    {
        return YAML::Node(nb::cast<double>(obj));
    }
    if (nb::isinstance<nb::str>(obj))
    {
        return YAML::Node(nb::cast<std::string>(obj));
    }
    if (nb::isinstance<nb::dict>(obj))
    {
        YAML::Node node(YAML::NodeType::Map);
        for (auto [key, value]: nb::borrow<nb::dict>(obj))
        {
            node[nb::cast<std::string>(nb::str(key))] = to_yaml(value);
        }
        return node;
    }
    if (nb::isinstance<nb::list>(obj))
    {
        YAML::Node node(YAML::NodeType::Sequence);
        for (nb::handle item: nb::borrow<nb::list>(obj))
        {
            node.push_back(to_yaml(item));
        }
        return node;
    }
    if (nb::isinstance<nb::tuple>(obj))
    {
        YAML::Node node(YAML::NodeType::Sequence);
        for (nb::handle item: nb::borrow<nb::tuple>(obj))
        {
            node.push_back(to_yaml(item));
        }
        return node;
    }
    throw std::runtime_error("unsupported Python type in uFW config spec");
}

// The built-in loaders, registered by name (mirrors main.cpp's add<T>("LIBRARY")).
enum class loader_kind { library, plugin };

void add_loader(application& app, std::string const& name, loader_kind kind)
{
    switch (kind)
    {
    case loader_kind::library: app.add<library_repository>(name); break;
    case loader_kind::plugin:  app.add<plugin_repository>(name);  break;
    }
}

void load_spec(application& app, nb::dict const& spec)
{
    app.load(to_yaml(spec).as<application_config>());
}

} // namespace

NB_MODULE(ufw, m)
{
    m.doc() = "uFW control plane — assemble and drive an application from Python";

    initialize_logger(); // start the Quill backend thread once, on import

    nb::enum_<loader_kind>(m, "Loader")
        .value("LIBRARY", loader_kind::library)
        .value("PLUGIN", loader_kind::plugin);

    nb::class_<application>(m, "application")
        .def(nb::init<>())
        .def("add_loader", &add_loader, nb::arg("name"), nb::arg("kind"),
             "Register a built-in loader (Loader.LIBRARY / Loader.PLUGIN) under a name.")
        .def("load", &load_spec, nb::arg("spec"),
             "Assemble entities from a spec dict ({'entities': [...]}) and lock the structure.")
        .def("run", &application::run, nb::call_guard<nb::gil_scoped_release>(),
             "Drive the io_context until shutdown (releases the GIL while running).")
        .def("shutdown", &application::shutdown,
             "Stop the io_context and unwind the lifecycle in reverse.");
}
