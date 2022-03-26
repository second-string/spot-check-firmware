if(IDF_VERSION_MAJOR GREATER_EQUAL 4)
    idf_component_register(SRC_DIRS ./
        # REQUIRES freertos
        INCLUDE_DIRS ./)
else()
    set(COMPONENT_SRCDIRS ./)
    set(COMPONENT_ADD_INCLUDEDIRS ./)
    # set(COMPONENT_REQUIRES freertos)
    register_component()
endif()
