# Configure with -G Xcode (and -DCMAKE_SYSTEM_NAME=tvOS for tvOS); see tools/ios/build.sh (profile step).
cmake_minimum_required(VERSION 3.25)
if(NOT CMAKE_SYSTEM_NAME)
    set(CMAKE_SYSTEM_NAME iOS)
endif()
project(botsprofile LANGUAGES C OBJC)
set(BUNDLE_ID "" CACHE STRING "Bundle identifier")
set(TEAM_ID "" CACHE STRING "Apple development team")
set(ENTITLEMENTS "" CACHE FILEPATH "Entitlements file")
set(BUILD_NUMBER 1)
string(TOLOWER "${CMAKE_SYSTEM_NAME}" _platform)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../Info.${_platform}.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
add_executable(bots MACOSX_BUNDLE main.m)
set_target_properties(bots PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID}
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ${TEAM_ID}
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS ${ENTITLEMENTS})
if(_platform STREQUAL "tvos")
    set_target_properties(bots PROPERTIES XCODE_ATTRIBUTE_TVOS_DEPLOYMENT_TARGET 17.0
                                          XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY 3)
else()
    set_target_properties(bots PROPERTIES XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 17.0
                                          XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
endif()
target_link_libraries(bots PRIVATE "-framework UIKit")
