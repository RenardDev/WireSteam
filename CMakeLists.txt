cmake_minimum_required(VERSION 3.20)
project(WireSteam VERSION 0.4.0 LANGUAGES CXX RC)

if(MSVC AND CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION VERSION_LESS "10.0.26100.0")
	message(FATAL_ERROR "WireSteam requires Windows SDK 10.0.26100.0 or later to build the per-application routing API.")
endif()

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "WireSteam requires Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(STEAMWORKS_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/sdk" CACHE PATH "Steamworks SDK 1.65 root")
if(NOT EXISTS "${STEAMWORKS_SDK_ROOT}/redistributable_bin/win64/steam_api64.lib")
	message(FATAL_ERROR "Steamworks x64 SDK not found at ${STEAMWORKS_SDK_ROOT}")
endif()

add_library(WireSteamPlatform STATIC src/platform.cpp src/config.cpp src/port_forward.cpp src/service_diagnostics.cpp
	src/internet_worker.cpp src/internet_routing.cpp src/internet_legacy_routes.cpp src/internet_server.cpp)
target_include_directories(WireSteamPlatform PUBLIC src)
target_compile_definitions(WireSteamPlatform PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(WireSteamPlatform PUBLIC ws2_32 advapi32 shell32 ole32 fwpuclnt iphlpapi)

add_executable(WireSteam src/main.cpp src/steam_bridge.cpp src/steam_identity.cpp src/ui_control.cpp src/preflight.cpp src/transport/protocol.cpp
	src/transport/udp_fragments.cpp src/transport_tests.cpp resources/WireSteam.rc)
target_include_directories(WireSteam PRIVATE src src/transport)
target_include_directories(WireSteam SYSTEM PRIVATE "${STEAMWORKS_SDK_ROOT}/public")
target_link_libraries(WireSteam PRIVATE WireSteamPlatform iphlpapi "${STEAMWORKS_SDK_ROOT}/redistributable_bin/win64/steam_api64.lib")

add_executable(WireSteamService src/service_main.cpp resources/WireSteamService.rc)
target_link_libraries(WireSteamService PRIVATE WireSteamPlatform)

add_library(WireSteamInstallerGuard SHARED installer/InstallerGuard.cpp)
target_compile_definitions(WireSteamInstallerGuard PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(WireSteamInstallerGuard PRIVATE msi advapi32 shell32)

add_dependencies(WireSteam WireSteamService)

add_library(WireSteamTestCommon STATIC src/test_apps/test_apps_common.cpp)
target_include_directories(WireSteamTestCommon PUBLIC src/test_apps src/transport)
target_compile_definitions(WireSteamTestCommon PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(WireSteamTestCommon PUBLIC ws2_32)

add_executable(WireSteamTestServer src/test_apps/test_server.cpp)

add_executable(WireSteamTestClient src/test_apps/test_client.cpp)
target_link_libraries(WireSteamTestServer PRIVATE WireSteamTestCommon)
target_link_libraries(WireSteamTestClient PRIVATE WireSteamTestCommon)

add_library(WireSteamSpeedCommon STATIC src/speed/speed_common.cpp)
target_include_directories(WireSteamSpeedCommon PUBLIC src/speed src/transport)
target_compile_definitions(WireSteamSpeedCommon PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(WireSteamSpeedCommon PUBLIC ws2_32 bcrypt)

add_executable(WireSteamSpeedServer src/speed/speed_server.cpp)

add_executable(WireSteamSpeedClient src/speed/speed_client.cpp)
target_link_libraries(WireSteamSpeedServer PRIVATE WireSteamSpeedCommon)
target_link_libraries(WireSteamSpeedClient PRIVATE WireSteamSpeedCommon)

foreach(target WireSteamPlatform WireSteam WireSteamService WireSteamInstallerGuard WireSteamTestCommon WireSteamTestServer WireSteamTestClient WireSteamSpeedCommon WireSteamSpeedServer WireSteamSpeedClient)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 "$<$<COMPILE_LANGUAGE:CXX>:/WX>" /utf-8 /permissive- /EHsc /guard:cf)
		target_link_options(${target} PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
	endif()
endforeach()

add_custom_command(TARGET WireSteam POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${STEAMWORKS_SDK_ROOT}/redistributable_bin/win64/steam_api64.dll" "$<TARGET_FILE_DIR:WireSteam>"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/steam_appid.txt" "$<TARGET_FILE_DIR:WireSteam>")

install(TARGETS WireSteam WireSteamService WireSteamTestServer WireSteamTestClient WireSteamSpeedServer WireSteamSpeedClient RUNTIME DESTINATION .)
install(FILES "${STEAMWORKS_SDK_ROOT}/redistributable_bin/win64/steam_api64.dll" steam_appid.txt README.md CONFIGURATION.md SPEEDTEST.md INTERNET.md INSTALLER.md FORWARDING.md THIRD_PARTY_NOTICES.md LICENSE DESTINATION .)
install(FILES docs/images/overview.png DESTINATION docs/images)
install(DIRECTORY examples DESTINATION .)

option(WIRESTEAM_BUILD_UI "Build the self-contained Windows desktop UI (.NET 10 SDK required)" ON)
if(WIRESTEAM_BUILD_UI)
	find_program(WIRESTEAM_DOTNET NAMES dotnet.exe dotnet HINTS "${CMAKE_CURRENT_SOURCE_DIR}/build-dotnet" NO_DEFAULT_PATH)
	if(NOT WIRESTEAM_DOTNET)
		find_program(WIRESTEAM_DOTNET NAMES dotnet.exe dotnet REQUIRED)
	endif()

	execute_process(COMMAND "${CMAKE_COMMAND}" -E env DOTNET_GENERATE_ASPNET_CERTIFICATE=false
		DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 DOTNET_CLI_UI_LANGUAGE=en
		"${WIRESTEAM_DOTNET}" --version OUTPUT_VARIABLE WIRESTEAM_DOTNET_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
	if(WIRESTEAM_DOTNET_VERSION VERSION_LESS "10.0")
		message(FATAL_ERROR "The desktop UI requires the .NET 10 SDK. Set WIRESTEAM_DOTNET to its executable, or WIRESTEAM_BUILD_UI=OFF for CLI only.")
	endif()

	set(WIRESTEAM_UI_OUTPUT "${CMAKE_BINARY_DIR}/gui-publish")
	add_custom_target(WireSteamUI ALL
		COMMAND "${CMAKE_COMMAND}" -E env DOTNET_GENERATE_ASPNET_CERTIFICATE=false
			DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 DOTNET_CLI_UI_LANGUAGE=en
			"${WIRESTEAM_DOTNET}" publish "${CMAKE_CURRENT_SOURCE_DIR}/gui/WireSteam.UI.csproj"
			--configuration "$<CONFIG>" --runtime win-x64 --self-contained true --output "${WIRESTEAM_UI_OUTPUT}" --nologo
		BYPRODUCTS "${WIRESTEAM_UI_OUTPUT}/WireSteam.UI.exe"
		COMMENT "Publishing the self-contained WireSteam desktop UI" VERBATIM)
	add_dependencies(WireSteamUI WireSteam WireSteamService)
	install(FILES "${WIRESTEAM_UI_OUTPUT}/WireSteam.UI.exe" GUI.md DESTINATION .)
endif()

include(CTest)

if(BUILD_TESTING)
	add_executable(WireSteamPortForwardTests src/port_forward_tests.cpp)
	target_link_libraries(WireSteamPortForwardTests PRIVATE WireSteamPlatform)

	if(MSVC)
		target_compile_options(WireSteamPortForwardTests PRIVATE /W4 "$<$<COMPILE_LANGUAGE:CXX>:/WX>" /utf-8 /permissive- /EHsc /guard:cf)
		target_link_options(WireSteamPortForwardTests PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
	endif()

	add_test(NAME WireSteam.PortForward COMMAND WireSteamPortForwardTests)
	set_tests_properties(WireSteam.PortForward PROPERTIES TIMEOUT 90)

	add_executable(WireSteamInstallerGuardTests installer/InstallerGuardTests.cpp)
	target_compile_definitions(WireSteamInstallerGuardTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
	target_link_libraries(WireSteamInstallerGuardTests PRIVATE msi advapi32 shell32)

	if(MSVC)
		target_compile_options(WireSteamInstallerGuardTests PRIVATE /W4 "$<$<COMPILE_LANGUAGE:CXX>:/WX>" /utf-8 /permissive- /EHsc /guard:cf)
		target_link_options(WireSteamInstallerGuardTests PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
	endif()

	add_test(NAME WireSteam.InstallerGuard COMMAND WireSteamInstallerGuardTests)
	set_tests_properties(WireSteam.InstallerGuard PROPERTIES TIMEOUT 15)

	if(WIRESTEAM_BUILD_UI)
		add_test(NAME WireSteam.DesktopUI COMMAND "${WIRESTEAM_UI_OUTPUT}/WireSteam.UI.exe" --self-test --engine "$<TARGET_FILE:WireSteam>")
		set_tests_properties(WireSteam.DesktopUI PROPERTIES TIMEOUT 90)
	endif()

	add_test(NAME WireSteam.Core COMMAND WireSteam self-test)
	set_tests_properties(WireSteam.Core PROPERTIES TIMEOUT 90)

	add_test(NAME WireSteam.Help COMMAND WireSteam --help)

	add_test(NAME WireSteam.Version COMMAND WireSteam --version)

	add_test(NAME WireSteam.TestServerHelp COMMAND WireSteamTestServer --help)

	add_test(NAME WireSteam.TestClientHelp COMMAND WireSteamTestClient --help)

	find_program(WIRESTEAM_POWERSHELL NAMES pwsh powershell REQUIRED)

	add_test(NAME WireSteam.TestApplications COMMAND "${WIRESTEAM_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
		-File "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_apps.ps1" -ServerPath "$<TARGET_FILE:WireSteamTestServer>" -ClientPath "$<TARGET_FILE:WireSteamTestClient>")
	set_tests_properties(WireSteam.TestApplications PROPERTIES TIMEOUT 90)

	add_test(NAME WireSteam.CLI COMMAND "${WIRESTEAM_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
		-File "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.ps1" -WireSteamPath "$<TARGET_FILE:WireSteam>")
	set_tests_properties(WireSteam.CLI PROPERTIES TIMEOUT 60)

	add_test(NAME WireSteam.UiControlCLI COMMAND "${WIRESTEAM_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
		-File "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_control.ps1" -WireSteamPath "$<TARGET_FILE:WireSteam>")
	set_tests_properties(WireSteam.UiControlCLI PROPERTIES TIMEOUT 60)

	add_test(NAME WireSteam.AdapterPreflight COMMAND "${WIRESTEAM_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
		-File "${CMAKE_CURRENT_SOURCE_DIR}/tests/preflight.ps1" -WireSteamPath "$<TARGET_FILE:WireSteam>")
	set_tests_properties(WireSteam.AdapterPreflight PROPERTIES TIMEOUT 60)

	if(WIRESTEAM_BUILD_UI)
		add_test(NAME WireSteam.WindowsCompatibility COMMAND "${WIRESTEAM_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
			-File "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_compatibility.ps1" -WireSteamPath "$<TARGET_FILE:WireSteam>"
			-ServicePath "$<TARGET_FILE:WireSteamService>" -UiPath "${WIRESTEAM_UI_OUTPUT}/WireSteam.UI.exe")
	else()
		add_test(NAME WireSteam.WindowsCompatibility COMMAND "${WIRESTEAM_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
			-File "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_compatibility.ps1" -WireSteamPath "$<TARGET_FILE:WireSteam>"
			-ServicePath "$<TARGET_FILE:WireSteamService>")
	endif()

	set_tests_properties(WireSteam.WindowsCompatibility PROPERTIES TIMEOUT 30)

	add_test(NAME WireSteam.SpeedUnit COMMAND WireSteamSpeedClient --self-test)
	set_tests_properties(WireSteam.SpeedUnit PROPERTIES TIMEOUT 30)

	add_test(NAME WireSteam.SpeedIntegration COMMAND "${WIRESTEAM_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
		-File "${CMAKE_CURRENT_SOURCE_DIR}/tests/speed.ps1" -ServerPath "$<TARGET_FILE:WireSteamSpeedServer>" -ClientPath "$<TARGET_FILE:WireSteamSpeedClient>")
	set_tests_properties(WireSteam.SpeedIntegration PROPERTIES TIMEOUT 120)

	add_test(NAME WireSteam.SpeedConcurrent COMMAND "${WIRESTEAM_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
		-File "${CMAKE_CURRENT_SOURCE_DIR}/tests/speed_concurrent.ps1" -ServerPath "$<TARGET_FILE:WireSteamSpeedServer>" -ClientPath "$<TARGET_FILE:WireSteamSpeedClient>")
	set_tests_properties(WireSteam.SpeedConcurrent PROPERTIES TIMEOUT 120)
endif()
