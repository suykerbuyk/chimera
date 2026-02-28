# Windows Protocol Test Suites

This directory contains an SnTest that runs the SMB2 Server Tests from
the Windows Protocol Test Suites against a Chimera server. This test suite
is maintained by Microsoft and is hosted on Github at
https://github.com/microsoft/WindowsProtocolTestSuites.

There are a total of 248 tests in the SMB2 Server sub-suite. However, only
only 64 of these pass when run against a Samba server. The rest are skipped
or fail. At some point, we should investigate why so few pass. However,
for now, the test here only attempts to run the 64 tests against Chimera with
the assumption being that the others will also fail or be skipped.

# Invocation

The most basic way to run the test is the following:

sntest -b master -s win-proto-test-suite-chimera.snt -vPKG_TAG=<tag>

This will run the 64 tests of which most currently fail.

However, some of these tests are known to pass. These can be run using:

sntest -b master -s win-proto-test-suite-chimera.snt -vPKG_TAG=<tag>,RUN_WORKING=1

It is also possible to run specifc tests. For example, to run
InvalidCreateRequestStructureSize and BVT_SessionMgmt_NewSession, use:

sntest "-vTEST_LIST=InvalidCreateRequestStructureSize BVT_SessionMgmt_NewSession,PKG_TAG=jhabermann" -s win-proto-test-suite-chimera.snt -b master
