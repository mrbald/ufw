/* 
 * Copyright (c) 2015-2023 Vladimir Lysyy (mrbald@github)
 * ALv2 (http://www.apache.org/licenses/LICENSE-2.0)
 *
 * 2015-03-28 - Vladimir Lysyy - Initial version
 * 2023-09-17 - Vladimir Lysyy - Tech refresh
 */

#pragma once

#include "entity.hpp"

#include <string>
#include <memory>
#include <functional>
#include <stdexcept>
#include <type_traits>

#include <dlfcn.h>

namespace ufw {

/* 
 * Dynamic Library Abstraction
 */
struct library final: std::enable_shared_from_this<library> {
    static std::shared_ptr<library> load(char const* path);

    ~library() noexcept
    {
        if (handle_) dlclose(handle_);
    }

    template <class F>
    std::function<F> function(std::string const& name) const
    {
        if (!handle_) throw std::runtime_error("library not open");
        dlerror();
        auto fp = reinterpret_cast<typename std::add_pointer<F>::type>(dlsym(handle_, name.c_str()));
        const char *dlsym_error = dlerror();
        if (dlsym_error) throw std::runtime_error(dlsym_error);

        // aliased shared_ptr to hold the library alive until all references
        // to functions loaded from it are disposed
        return func_helper<F>::create(std::shared_ptr<F>(shared_from_this(), fp));
    }

    // ctors are not to be called directly, static method create() should be
    // used instead
    library(char const* path):
        handle_(dlopen(path, RTLD_LAZY))
    {
        if (!handle_) throw std::runtime_error(dlerror());
    }

private:
    template <class F> struct func_helper;
    template <class R, class... Args> struct func_helper <R(Args...)>
    {
        static std::function<R(Args...)> create(std::shared_ptr<R(Args...)> ptr)
        {
            return [=](Args&&... args) { return (**ptr)(std::forward<Args>(args)...); };
        }
    };

    void* handle_ = nullptr;
};
using library_ptr = std::shared_ptr<library>;

inline library_ptr library::load(char const* path)
{
    return std::make_shared<library>(path);
}

struct library_entity: entity {
    template <class... Args>
    library_entity(library_ptr library, Args&&... args):
            entity {std::forward<Args>(args)...},
            library_ {std::move(library)} {}

    template <class F>
    std::function<F> function(std::string const& name) const
    {
        return library_->template function<F>(name);
    }

private:
    library_ptr const library_;
};

} // namespace ufw
