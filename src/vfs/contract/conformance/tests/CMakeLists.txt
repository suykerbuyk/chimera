# SPDX-FileCopyrightText: 2026 Quantum Corporation
#
# SPDX-License-Identifier: MIT

# Harness self-test: proves the driver, the deferred-completion fixture and the
# obligation vocabulary all work -- the honest fixture passes every obligation
# and each cheat profile makes the matching obligation fail.  It is in-process
# and unprivileged, so it runs in the quick tier and never skips.
set(CHIMERA_TEST_TIER quick)

add_executable(sc_conformance_selftest sc_conformance_selftest.c)
target_link_libraries(sc_conformance_selftest
    chimera_sc_conformance chimera_sc_fixture evpl)
if(MSVC)
    target_compile_options(sc_conformance_selftest PRIVATE /UNDEBUG)
else()
    target_compile_options(sc_conformance_selftest PRIVATE -UNDEBUG)
endif()
add_test(NAME chimera/contract/conformance/selftest COMMAND sc_conformance_selftest)
set_tests_properties(chimera/contract/conformance/selftest PROPERTIES TIMEOUT 60)
