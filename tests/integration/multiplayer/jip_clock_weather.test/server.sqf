triAssertNgs 14
triAssertNgsClient 14
triAssertEq [format["%1", clockEarlyReady], "1"]

// Move the world on before the late client exists. Five hours, and 0.75 overcast, so
// that a late joiner who fell back to the mission file lands somewhere no tolerance can
// call equal.
skipTime 5
0 setOvercast 0.75
0 setFog 0.4

// What the late client must agree with. Sent by publicVariable rather than recomputed,
// because the clock keeps advancing: both sides have to compare against ONE number, and
// the residual drift between the send and the assert is what the tolerance is for.
clockServerDayTime = dayTime
publicVariable "clockServerDayTime"
clockServerReady = 1
publicVariable "clockServerReady"

triAssertEq [format["%1", clockLateDone], "1"]
triWait 500
triEndTest
