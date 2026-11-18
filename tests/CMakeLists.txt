add_test(NAME openrig_package_validation COMMAND logonomic_cli_addon_validate
    "${OPENRIG_PACKAGE_DIRECTORY}" --platform "${OPENRIG_PLATFORM}")
add_test(NAME openrig_package_conformance COMMAND logonomic_cli_addon_conformance
    "${OPENRIG_PACKAGE_DIRECTORY}")

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME openrig_launcher_protocol COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/launcher_protocol_tests.py"
    "$<TARGET_FILE:openrig_adapter>" "${OPENRIG_PLATFORM}")
set_tests_properties(
    openrig_package_validation openrig_package_conformance openrig_launcher_protocol
    PROPERTIES TIMEOUT 60 LABELS "cli-addon;provider-contract")
