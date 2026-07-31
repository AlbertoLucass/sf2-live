if(NOT TARGET oboe::oboe)
add_library(oboe::oboe SHARED IMPORTED)
set_target_properties(oboe::oboe PROPERTIES
    IMPORTED_LOCATION "/home/oxeanbits/.gradle/caches/8.13/transforms/004d258255fe795595def01c20ca3190/transformed/oboe-1.10.0/prefab/modules/oboe/libs/android.arm64-v8a/liboboe.so"
    INTERFACE_INCLUDE_DIRECTORIES "/home/oxeanbits/.gradle/caches/8.13/transforms/004d258255fe795595def01c20ca3190/transformed/oboe-1.10.0/prefab/modules/oboe/include"
    INTERFACE_LINK_LIBRARIES ""
)
endif()

