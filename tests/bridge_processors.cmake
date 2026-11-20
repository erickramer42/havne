# Included by CTest after test_bridge's discovered tests: the cases that
# build and read denseRecording() are CPU-bound, so each takes two of -j's slots.
foreach(_name
        AStatisticsAskDuringAMeansPassIsAnsweredBeforeItEnds
        ANewerMeansAskReplacesAPassThatGaveWay
        ANewerMeansAskTakenWithTheAskAPassGaveWayToReplacesIt
        ANewerStretchesAskStalesTheOlderPass
        AStretchesAskThatCantStartLeavesTheOlderPassAlone
        ACloseEndsAMeansPassThatGaveWay
        ACloseStopsAStretchesPassUnderWay
        AStopInAPassesSecondCallEndsIt)
    if(TEST RecordingViewTest.${_name})
        set_tests_properties(RecordingViewTest.${_name} PROPERTIES PROCESSORS 2)
    endif()
endforeach()
