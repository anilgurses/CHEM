file(GLOB chem_node_sources "src/chem/node/*.cpp")
file(GLOB chem_channel_sources "src/chem/channel/*.cpp")
file(GLOB chem_dsp_sources "src/chem/dsp/*.cpp")
file(GLOB chem_vehicle_sources "src/chem/vehicle/*.cpp")
file(GLOB chem_net_sources "src/chem/net/*.cpp")
file(GLOB chem_extension_core_sources "src/chem/extensions/*.cpp")
file(GLOB chem_antenna_sources "src/chem/antennas/generated/*.cpp")

# Auto-discover extensions under extensions/*/
set(chem_extension_sources "")
set(chem_extension_headers "")
set(chem_extension_include_dirs "")
file(GLOB extension_dirs "extensions/*")
foreach(ext_dir ${extension_dirs})
    if(IS_DIRECTORY ${ext_dir})
        file(GLOB _ext_srcs "${ext_dir}/src/*.cpp")
        file(GLOB _ext_hdrs "${ext_dir}/include/*.h")
        list(APPEND chem_extension_sources ${_ext_srcs})
        list(APPEND chem_extension_headers ${_ext_hdrs})
        if(EXISTS "${ext_dir}/include")
            list(APPEND chem_extension_include_dirs "${ext_dir}/include")
        endif()
        # NOTE: Maybe per extension cmake can be added, IDK just loud thinking
    endif()
endforeach()

set(sources
    src/chem/config.cpp
    src/chem/db/dbHandler.cpp
    ${chem_node_sources}
    ${chem_channel_sources}
    ${chem_dsp_sources}
    ${chem_vehicle_sources}
    ${chem_net_sources}
    ${chem_extension_core_sources}
    ${chem_extension_sources}
    ${chem_antenna_sources}
)

set(exe_sources src/chem/main.cpp ${sources})

# 3rd party
file(GLOB spdlog_headers "include/spdlog/*.h")
file(GLOB json_headers "include/nlohmann/*.hpp")

# CHEM
file(GLOB chem_headers "include/chem/*.h")
file(GLOB chem_dsp_headers "include/chem/dsp/*.h")
file(GLOB chem_models_headers "include/chem/models/*.h")
file(GLOB chem_db_headers "include/chem/db/*.h")
file(GLOB chem_node_headers "include/chem/node/*.h")
file(GLOB chem_channel_headers "include/chem/channel/*.h")
file(GLOB chem_vehicle_headers "include/chem/vehicle/*.h")
file(GLOB chem_net_headers "include/chem/net/*.h")
file(GLOB chem_extension_core_headers "include/chem/extensions/*.h")

set(headers
    ${spdlog_headers}
    ${json_headers}
    include/nlohmann/json.hpp
    ${chem_headers}
    ${chem_dsp_headers}
    ${chem_models_headers}
    ${chem_db_headers}
    ${chem_node_headers}
    ${chem_channel_headers}
    ${chem_vehicle_headers}
    ${chem_net_headers}
    ${chem_extension_core_headers}
    ${chem_extension_headers}
)

set(test_sources
    src/dsp_channel.cpp
    src/channel.cpp
    src/intermediate.cpp
)
