if(IDF_VERSION_MAJOR GREATER_EQUAL 4)
    idf_component_register(SRC_DIRS ./
        REQUIRES log freertos
        INCLUDE_DIRS ./include)
else()
    set(COMPONENT_SRCDIRS ./)
    set(COMPONENT_ADD_INCLUDEDIRS ./include)
    set(COMPONENT_REQUIRES log freertos)
    register_component()
endif()
