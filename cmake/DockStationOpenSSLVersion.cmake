# Keep OpenSSL source/API compatibility explicit and shared by the build
# and policy tests. This does not determine whether a branch receives security fixes.
function(dockstation_openssl_version_supported version result_variable)
    if("${version}" MATCHES "^3\\.[0-9]+\\.[0-9]+([.+-].*)?$")
        set(${result_variable} TRUE PARENT_SCOPE)
    else()
        set(${result_variable} FALSE PARENT_SCOPE)
    endif()
endfunction()
