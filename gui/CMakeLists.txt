qt_standard_project_setup()

qt_add_executable(sf100gui MACOSX_BUNDLE
    main.cpp
    mainwindow.h
    mainwindow.cpp
    coreworker.h
    coreworker.cpp
    hexview.h
    hexview.cpp
    configdialog.h
    configdialog.cpp
    chipselectdialog.h
    chipselectdialog.cpp
    statusregdialog.h
    statusregdialog.cpp
)
set_target_properties(sf100gui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(sf100gui PRIVATE Qt6::Widgets sf100core)
target_compile_options(sf100gui PRIVATE -Wall)

# The engine looks for ChipInfoDb.dedicfg next to the executable, or in <exe_dir>/../Resources (macOS bundle).
if(APPLE)
    add_custom_command(TARGET sf100gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:sf100gui>/Resources
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${ENGINE_DIR}/ChipInfoDb.dedicfg $<TARGET_BUNDLE_CONTENT_DIR:sf100gui>/Resources)
else()
    add_custom_command(TARGET sf100gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${ENGINE_DIR}/ChipInfoDb.dedicfg $<TARGET_FILE_DIR:sf100gui>)
endif()

install(TARGETS sf100gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} BUNDLE DESTINATION .)
