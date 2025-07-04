cmake_minimum_required(VERSION 3.13)

#玲珑构建依赖目录
include_directories(${CMAKE_INSTALL_PREFIX}/include)


set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIE")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pie")

set(APP_BIN_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/bin/)
set(BIN_NAME ${CMAKE_PROJECT_NAME})

list(APPEND RESOURCES)

find_package(Qt${QT_VERSION_MAJOR} CONFIG REQUIRED COMPONENTS
    Qml
    Quick
    DBus
    Concurrent
    Svg
    PrintSupport
    LinguistTools
    Sql
)

find_package(Dtk${DTK_VERSION_MAJOR} REQUIRED COMPONENTS
    Widget
    Gui
    Declarative
)

# 三方库
find_package(PkgConfig REQUIRED)
pkg_check_modules(3rd_lib REQUIRED
     #[[gio-qt udisks2-qt5]] libavformat
    )
pkg_check_modules(dfmmount REQUIRED
    dfm${DTK_VERSION_MAJOR}-mount
    )

set(DFM_MOUNT_HEADERS ${dfmmount_INCLUDE_DIRS})
include_directories(${DFM_MOUNT_HEADERS})

# 保证 src 目录下头文件全局可见
include_directories(src)

# 源文件
file(GLOB_RECURSE SRCS CONFIGURE_DEPENDS "./*.h" "./*.cpp")
file(GLOB_RECURSE QMLSRC ./*.qml)

if(NOT BUILD_WITH_QT6)
    find_package(Qt5QuickCompiler CONFIG REQUIRED)
    qtquick_compiler_add_resources(RCC_SOURCES ${RESOURCES})
endif()

if(NOT(${CMAKE_BUILD_TYPE} MATCHES "Debug"))
    # 调整优化级别
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")

    # Translation
    file(GLOB TS LIST_DIRECTORIES false translations/${CMAKE_PROJECT_NAME}*.ts)
    set_source_files_properties(${TS} PROPERTIES OUTPUT_LOCATION ${PROJECT_SOURCE_DIR}/translations)
    qt_create_translation(QM ${SRCS} ${QMLSRC} ${TS})
endif()

# 仅生成 EXE 文件，用以兼容新(5.6.0+dev-1及以上)旧两版本DtkDeclarative库
add_executable(${BIN_NAME}
    ${SRCS}
    ${RCC_SOURCES}
    ${QM}
    ${CMAKE_PROJECT_NAME}.qrc
    res.qrc
)

target_include_directories(${BIN_NAME} PUBLIC ${3rd_lib_INCLUDE_DIRS}  )
target_link_libraries(${BIN_NAME}
    Qt${QT_VERSION_MAJOR}::Quick
    Qt${QT_VERSION_MAJOR}::PrintSupport
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Qml
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::DBus
    Qt${QT_VERSION_MAJOR}::Concurrent
    Qt${QT_VERSION_MAJOR}::Svg
    Qt${QT_VERSION_MAJOR}::Sql

    Dtk${DTK_VERSION_MAJOR}::Widget
    Dtk${DTK_VERSION_MAJOR}::Gui
    Dtk${DTK_VERSION_MAJOR}::Declarative
    GL
    pthread
    ${3rd_lib_LIBRARIES}
    ${dfmmount_LIBRARIES}
    )

if (${CMAKE_BUILD_TYPE} MATCHES "Debug")
    TARGET_COMPILE_DEFINITIONS(${BIN_NAME} PRIVATE $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:QT_QML_DEBUG>)
endif ()

set_target_properties(${BIN_NAME} PROPERTIES INSTALL_RPATH ${DTK_QML_APP_PLUGIN_PATH})

set(PREFIX ${CMAKE_INSTALL_PREFIX})
set(AppIconDir ${PREFIX}/share/icons/hicolor/scalable/apps)
set(APPSHAREDIR  ${CMAKE_INSTALL_PREFIX}/share/${CMAKE_PROJECT_NAME})

# Add install rule
install(TARGETS ${BIN_NAME} DESTINATION ${APP_BIN_INSTALL_DIR})
install(FILES ${PROJECT_SOURCE_DIR}/src/${CMAKE_PROJECT_NAME}.desktop DESTINATION share/applications)
install(FILES ${PROJECT_SOURCE_DIR}/src/icon/deepin-album.svg DESTINATION ${CMAKE_INSTALL_PREFIX}/share/icons/hicolor/scalable/apps)
install(FILES com.deepin.album.service DESTINATION ${CMAKE_INSTALL_PREFIX}/share/dbus-1/services/)

install(TARGETS ${LIB_NAME} DESTINATION ${DTK_QML_APP_PLUGIN_PATH})
install(TARGETS ${LIB_NAME} DESTINATION ${PREFIX}/plugins/imageformats)

#translations
install(DIRECTORY ${PROJECT_SOURCE_DIR}/translations
    DESTINATION ${APPSHAREDIR}
    FILES_MATCHING PATTERN "*.qm")

#manual
install(DIRECTORY ${PROJECT_SOURCE_DIR}/src/assets/deepin-album
    DESTINATION ${CMAKE_INSTALL_PREFIX}/share/deepin-manual/manual-assets/application/
    FILES_MATCHING PATTERN "*")
