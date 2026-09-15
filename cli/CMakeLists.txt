add_executable(sb53-cli
    main.cpp
    HttpServer.cpp
    WebUI.cpp
)

target_link_libraries(sb53-cli
    PRIVATE sb53::core sb53_warnings
)

# Deliberately NOT "sb53" -- that sits next to the legacy SB53-Systems.exe in a folder
# listing and in the taskbar, and telling them apart at a glance matters when one of them
# has been validated by real prints and the other has not.
set(SB53_EXE_NAME "flowtemp" CACHE STRING "Name of the built executable")

set_target_properties(sb53-cli PROPERTIES
    OUTPUT_NAME "${SB53_EXE_NAME}"
)
