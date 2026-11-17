include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/DockStationOpenSSLVersion.cmake")

function(expect_version version expected)
    dockstation_openssl_version_supported("${version}" actual)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR
            "OpenSSL version policy mismatch for '${version}': expected ${expected}, got ${actual}")
    endif()
endfunction()

foreach(version IN ITEMS 3.0.0 3.0.13 3.5.0 3.5.5 3.99.99)
    expect_version("${version}" TRUE)
endforeach()

foreach(version IN ITEMS 1.1.1 2.0.0 4.0.0 4.1.0 3.0 3.x unknown)
    expect_version("${version}" FALSE)
endforeach()

message(STATUS "OpenSSL 3.x compatibility policy tests passed")
