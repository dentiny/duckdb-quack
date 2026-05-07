add_library(quack_storage OBJECT quack_catalog.cpp quack_insert.cpp)
set(EXTENSION_SOURCES
    ${EXTENSION_SOURCES} $<TARGET_OBJECTS:quack_storage>
    PARENT_SCOPE)
