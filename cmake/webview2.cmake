# The WebView2 SDK (Microsoft's NuGet package): downloaded once, checked against its SHA-256, unpacked into build/_deps.
# Gives the imported target `webview2` (headers + the static loader, so no WebView2Loader.dll ships). The browser itself
# is the WebView2 Runtime that comes with Windows 10 / 11.

set(WEBVIEW2_VERSION 1.0.4258.31)
set(WEBVIEW2_SHA256 56f7f4b8bf9aee4b8efefbbdd4f67d5f74ebd1b100ed0806da71bf76af481aa9)
set(WEBVIEW2_ROOT ${CMAKE_BINARY_DIR}/_deps/webview2-${WEBVIEW2_VERSION})

if(NOT EXISTS ${WEBVIEW2_ROOT}/build/native/include/WebView2.h)
  message(STATUS "Downloading the WebView2 SDK ${WEBVIEW2_VERSION}")
  file(DOWNLOAD https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}
       ${CMAKE_BINARY_DIR}/_deps/webview2-${WEBVIEW2_VERSION}.zip EXPECTED_HASH SHA256=${WEBVIEW2_SHA256})
  file(ARCHIVE_EXTRACT INPUT ${CMAKE_BINARY_DIR}/_deps/webview2-${WEBVIEW2_VERSION}.zip DESTINATION ${WEBVIEW2_ROOT})
endif()

add_library(webview2 INTERFACE)
target_include_directories(webview2 INTERFACE ${WEBVIEW2_ROOT}/build/native/include)
target_link_libraries(webview2 INTERFACE ${WEBVIEW2_ROOT}/build/native/x64/WebView2LoaderStatic.lib version)
