/*
   Copyright 2017 Vladimir Lysyy (mrbald@github)

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

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <string>
#include <vector>

namespace ufw {

using config_t = YAML::Node;

struct entity_config
{
    std::string name;
    std::string loader_ref;

    config_t config;
};

struct application_config
{
    std::vector<entity_config> entities;
};

} // namespace ufw

#define CFG_NAMESPACE YAML
#define CFG_ENCODE(name) do { node[#name] = rhs.name; } while (false)
#define CFG_ENCODE_IF_SET(name) do { if (!rhs.name.empty()) node[#name] = rhs.name; } while (false)
#define CFG_DECODE(name) do { rhs.name = node[#name].as<decltype(rhs.name)>(); } while (false)
#define CFG_DECODE_IF_SET(name) do { if (node[#name]) rhs.name = node[#name].as<decltype(rhs.name)>(); } while (false)

namespace CFG_NAMESPACE
{


template <>
struct convert<ufw::entity_config> {
    static Node encode(const ufw::entity_config& rhs) {
        Node node;

        CFG_ENCODE(name);
        CFG_ENCODE_IF_SET(loader_ref);
        CFG_ENCODE(config);

        return node;
    }

    static bool decode(const Node& node, ufw::entity_config& rhs)
    {
        if (node.IsSequence())
            return false;

        CFG_DECODE(name);
        CFG_DECODE_IF_SET(loader_ref);
        CFG_DECODE_IF_SET(config);

        return true;
    }
};

template <>
struct convert<ufw::application_config> {
    static Node encode(const ufw::application_config& rhs) {
        Node node;
        CFG_ENCODE(entities);
        return node;
    }

    static bool decode(const Node& node, ufw::application_config& rhs)
    {
        if (node.IsSequence())
            return false;
        CFG_DECODE(entities);
        return true;
    }
};

} // namespace CFG_NAMESPACE
