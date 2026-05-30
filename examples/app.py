#!/usr/bin/env python3
"""uFW control plane in Python — the equivalent of examples/app.yaml.

Build the bindings with:
    cmake ... -DUFW_BUILD_PYTHON=ON -DUFW_ENABLE_SANITIZERS=OFF
then point PYTHONPATH at the directory holding the compiled `ufw` module and the
loader at the directory holding libexample, e.g.:
    PYTHONPATH=build/Py/ufw/py DYLD_LIBRARY_PATH=build/Py/ufw/app \
        .venv/bin/python examples/app.py
"""
import ufw

app = ufw.application()

# Register the built-in loaders (mirrors main.cpp's add<library_repository>("LIBRARY")).
app.add_loader("LIBRARY", ufw.Loader.LIBRARY)
app.add_loader("PLUGIN", ufw.Loader.PLUGIN)

# The same entities examples/app.yaml describes — assembled as a Python dict.
app.load({
    "entities": [
        {"name": "LOGGER", "config": {
            "severity": "info",
            "pattern": "%(time) | %(log_level:<7) | %(thread_id) | %(logger) - %(message)",
            "timestamp_pattern": "%H:%M:%S.%Qms",
        }},
        {"name": "example_lib", "loader_ref": "LIBRARY", "config": {
            "filename": "libexample.dylib",  # libexample.so on Linux
        }},
        {"name": "example_plugin", "loader_ref": "PLUGIN", "config": {
            "library_ref": "example_lib",
            "constructor": "example_ctor",
        }},
    ]
})

app.run()  # blocks until the example schedules shutdown (~5s) or Ctrl-C
