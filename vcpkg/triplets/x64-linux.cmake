set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
# glib is shared because libprism.so can't carry it statically: inside libprism glib's constructor would run after those of glibmm's statics, which already use it.
if(PORT MATCHES "libgit" OR PORT MATCHES "libplist" OR PORT MATCHES "libarchive" OR PORT MATCHES "ethindp-prism" OR PORT STREQUAL "glib")
	set(VCPKG_LIBRARY_LINKAGE dynamic)
	set(VCPKG_FIXUP_ELF_RPATH ON)
else()
	set(VCPKG_LIBRARY_LINKAGE static)
endif()

set(VCPKG_CMAKE_SYSTEM_NAME Linux)
# Static libraries are built position independent, because some of them (libffi inside glib, glibmm and libspeechd inside Prism) are linked into shared libraries.
set(VCPKG_C_FLAGS -fPIC)
set(VCPKG_CXX_FLAGS -fPIC)
set(VCPKG_CMAKE_CONFIGURE_OPTIONS_RELEASE -DCMAKE_BUILD_TYPE=MinSizeRel)
