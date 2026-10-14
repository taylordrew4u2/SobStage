# ctest -P script: a take recorded on this computer for a card goes to that
# card and no other at the same place. The decision is cardcopy::copyTake's
# (unit-tested, and sim_card_copy section 13); this pins that Application
# hands it the card the take was armed with -- in the note beside the take
# and in the copy started at Stop -- and that the retry the status tick makes
# every 10 s includes a take waiting for its own card, so it is copied when
# that card is back.
#
#   -DAPPLICATION_CPP=<Source/App/Application.cpp>
if (NOT DEFINED APPLICATION_CPP OR "${APPLICATION_CPP}" STREQUAL "")
    message(FATAL_ERROR "Pass -DAPPLICATION_CPP=...")
endif()

file(READ "${APPLICATION_CPP}" text)

string(FIND "${text}" "currentCopyCardIdentity = onComputer ? cardIdentityAtDestination()" armed)
if (armed EQUAL -1)
    message(FATAL_ERROR "the arm no longer takes the card's identity from the probe")
endif()

string(FIND "${text}" "{ destinationFolder, {}, currentCopyCardIdentity }" note)
if (note EQUAL -1)
    message(FATAL_ERROR "the note beside the take no longer names its card")
endif()

string(FIND "${text}" "{ currentCopyRoot.toStdString(), {}, currentCopyCardIdentity }" job)
if (job EQUAL -1)
    message(FATAL_ERROR "the copy started at Stop no longer names the take's card")
endif()

string(FIND "${text}" "void Application::tickCardCopies()" start)
if (start EQUAL -1)
    message(FATAL_ERROR "tickCardCopies() not found")
endif()
string(SUBSTRING "${text}" ${start} -1 body)
string(FIND "${body}" "\n}\n" end)
string(SUBSTRING "${body}" 0 ${end} body)

string(FIND "${body}" "r.outcome == cardcopy::Outcome::DifferentCard" retried)
if (retried EQUAL -1)
    message(FATAL_ERROR "the status tick's retry no longer includes a take waiting for its own card")
endif()

message(STATUS "A take for a card is handed its card, and retried until that card is back")
