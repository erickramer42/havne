// File/layer: core/src/shared_core.cpp | C++17, Qt-free
// Testing: INTEGRATION: c_abi_shared, ExportsTest (tests/CMakeLists.txt)
//
// The shared core's own source (core/CMakeLists.txt, HAVNE_SHARED_CORE),
// empty on purpose: a library target needs one, and everything the
// library holds comes from ecuio-core's archive, linked whole. What it
// exports is scripts/ecuio_exports.py's list, made from the public
// headers; nothing is declared here, so nothing here can join it.
