cmake_minimum_required(VERSION 3.5)

project(CsTests LANGUAGES CXX)

find_package(QT NAMES Qt5 Qt6 COMPONENTS Test REQUIRED)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Test REQUIRED)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

add_executable(CsTests main.cpp ImporterTests.cpp ImporterTests.h ../src/services/Importer.cpp)
add_test(NAME CsTests COMMAND CsTests)

target_link_libraries(CsTests PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Concurrent Qt${QT_VERSION_MAJOR}::Svg Qt${QT_VERSION_MAJOR}::Network)

add_executable(TgaTests TgaTests.cpp
    ../src/utils/TgaCodec.cpp ../src/utils/ImageIO.cpp ../src/utils/Utils.cpp
    ../src/models/CImage.cpp ../src/services/Importer.cpp
    ../src/exceptions/ImageNotSupportedException.cpp ../src/exceptions/ImageTooBigException.cpp
    ../src/widgets/QCaesiumMessageBox.cpp)
set_property(TARGET TgaTests PROPERTY CXX_STANDARD 17)
target_link_libraries(TgaTests PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Concurrent Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Svg)
add_dependencies(TgaTests libcaesium)
if(WIN32)
    target_link_libraries(TgaTests PRIVATE ${LIBCAESIUM_EXTERNAL_PATH})
elseif(LIBCAESIUM)
    target_link_libraries(TgaTests PRIVATE ${LIBCAESIUM})
else()
    target_link_libraries(TgaTests PRIVATE caesium)
endif()
add_test(NAME TgaTests COMMAND TgaTests -o -,txt)

