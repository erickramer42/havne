# Included by CTest after test_j2534_connect's discovered tests. The J2534
# suites carry a 30 s ceiling; the out-of-memory loop takes about 26 s on a
# quiet machine and timed out once under load, so it gets 120 s. A bare
# set_tests_properties: `if(TEST ...)` is never true in a CTest include,
# and a name that no longer exists is ignored without a word, so
# CtestIncludeNamesTest.TheJ2534TimeoutNamesExist reads the list below.
foreach(_name
        OutOfMemoryAnywhereInCreateIsAnsweredNotThrown)
    set_tests_properties(J2534ConnectTest.${_name} PROPERTIES TIMEOUT 120)
endforeach()
