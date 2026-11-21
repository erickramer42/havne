# Included by CTest after test_bridge's discovered tests. The cases that
# build and read denseRecording() are CPU-bound, so each takes two of -j's
# slots. A bare set_tests_properties: `if(TEST ...)` is never true in a
# CTest include, and a name that no longer exists is ignored without a word.
foreach(_name
        AStatisticsAskDuringAMeansPassIsAnsweredBeforeItEnds
        ANewerMeansAskReplacesAPassThatGaveWay
        ANewerMeansAskTakenWithTheAskAPassGaveWayToReplacesIt
        ANewerStretchesAskStalesTheOlderPass
        AStretchesAskThatCantStartLeavesTheOlderPassAlone
        ACloseEndsAMeansPassThatGaveWay
        ACloseStopsAStretchesPassUnderWay
        AStopInAPassesSecondCallEndsIt)
    set_tests_properties(RecordingViewTest.${_name} PROPERTIES PROCESSORS 2)
endforeach()
