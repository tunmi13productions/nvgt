vcpkg_from_github(
  OUT_SOURCE_PATH SOURCE_PATH
  REPO ethindp/prism
  REF v0.18.3
  SHA512 a6cec5e04cf332dbe4ad75e8353559931fff6885b16f41304f73489f6dd236a8c014dc6b32f7d8c8838e4e39f2969c095403c5c8a5b8fd2218197f9eb35225fd
  HEAD_REF master
)
# Backends default to AUTO, building whatever the platform can. The two Linux backends that need libraries are switched by features, so vcpkg provides those libraries.
set(PRISM_ORCA OFF)
set(PRISM_SPEECH_DISPATCHER OFF)
if("orca" IN_LIST FEATURES)
  set(PRISM_ORCA ON)
endif()
if("speech-dispatcher" IN_LIST FEATURES)
  set(PRISM_SPEECH_DISPATCHER ON)
endif()
vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
  OPTIONS
    -DPRISM_ENABLE_TESTS=OFF
    -DPRISM_ENABLE_DEMOS=OFF
    -DPRISM_ENABLE_LINTING=OFF
    -DPRISM_ENABLE_GDEXTENSION=OFF
    -DPRISM_ENABLE_LEGACY_BACKENDS=ON
    -DPRISM_ENABLE_ORCA_BACKEND=${PRISM_ORCA}
    -DPRISM_ENABLE_SPEECH_DISPATCHER_BACKEND=${PRISM_SPEECH_DISPATCHER}
    # A static library built with link time optimization would carry compiler IR, forcing whoever links it to use LTCG.
    -DPRISM_IPO=OFF
)
vcpkg_cmake_install()
if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
  file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/bin" "${CURRENT_PACKAGES_DIR}/bin")
endif()
vcpkg_cmake_config_fixup(PACKAGE_NAME prism CONFIG_PATH share/prism)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_copy_pdbs()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
