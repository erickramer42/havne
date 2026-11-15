# cli/CMakeLists.txt
# havne-cli (ADR-0031): core's public headers and nothing else, so it
# builds in the core-only tree and the Qt-free CI jobs test it.
add_executable(havne-cli main.cpp)

find_package(Threads REQUIRED)
target_link_libraries(havne-cli PRIVATE ecuio-core Threads::Threads)
app_enable_warnings(havne-cli)
# layout check's reader recurses once a level, to 1024: Windows' 1 MB main
# stack holds about 1300 in a Debug build, so take Linux's and macOS's 8 MB
if(MSVC)
    target_link_options(havne-cli PRIVATE /STACK:8388608)
endif()
app_add_version_resource(havne-cli havne-cli "Havne command-line client")
install(TARGETS havne-cli RUNTIME DESTINATION bin)   # the core package (ADR-0035)

# A J2534 session needs the surrogates beside the exe. The app's build
# copies havne-cli next to havne.exe, where both are; this copies the
# x64 one here too, so the build tree's havne-cli runs the fake DLL.
if(WIN32 AND TARGET ecuio-j2534-surrogate)
    add_dependencies(havne-cli ecuio-j2534-surrogate)
    add_custom_command(TARGET havne-cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:ecuio-j2534-surrogate>
                $<TARGET_FILE_DIR:havne-cli>
        COMMENT "Deploying x64 J2534 surrogate next to havne-cli.exe"
        VERBATIM
    )
endif()
