file(GLOB_RECURSE lotusIR_graph_src
    "${LOTUS_INCLUDE_DIR}/core/graph/*.h"    
    "${LOTUS_ROOT}/core/graph/*.h"
    "${LOTUS_ROOT}/core/graph/*.cc"
)

file(GLOB_RECURSE lotusIR_defs_src
    "${LOTUS_ROOT}/core/defs/*.cc"
)

add_library(lotusIR_graph ${lotusIR_graph_src} ${lotusIR_defs_src})
add_dependencies(lotusIR_graph onnx_proto gsl)
lotus_add_include_to_target(lotusIR_graph onnx protobuf::libprotobuf)
target_include_directories(lotusIR_graph PRIVATE ${LOTUS_ROOT})
set_target_properties(lotusIR_graph PROPERTIES FOLDER "Lotus")
set_target_properties(lotusIR_graph PROPERTIES LINKER_LANGUAGE CXX)

source_group(TREE ${REPO_ROOT} FILES ${lotusIR_graph_src} ${lotusIR_defs_src})

if (WIN32)
    set(lotusIR_graph_static_library_flags
        -IGNORE:4221 # LNK4221: This object file does not define any previously undefined public symbols, so it will not be used by any link operation that consumes this library
    )
    
    set_target_properties(lotusIR_graph PROPERTIES
        STATIC_LIBRARY_FLAGS "${lotusIR_graph_static_library_flags}")
    
    target_compile_options(lotusIR_graph PRIVATE
        /EHsc   # exception handling - C++ may throw, extern "C" will not
    )

    # Add Code Analysis properties to enable C++ Core checks. Have to do it via a props file include. 
    set_target_properties(lotusIR_graph PROPERTIES VS_USER_PROPS ${PROJECT_SOURCE_DIR}/EnableVisualStudioCodeAnalysis.props)
endif()
