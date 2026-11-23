string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef suffix)
set(root "${TEST_ROOT}-${suffix}")
file(MAKE_DIRECTORY "${root}")
function(run_cli input expected)
    file(WRITE "${root}/input.txt" "${input}")
    execute_process(COMMAND "${CLI}" "${root}/db" INPUT_FILE "${root}/input.txt"
        OUTPUT_VARIABLE actual ERROR_VARIABLE error RESULT_VARIABLE status)
    string(REPLACE "\r\n" "\n" actual "${actual}")
    if(NOT status EQUAL 0 OR NOT actual STREQUAL expected)
        message(FATAL_ERROR "CLI failed (${status}): ${error}\nExpected:\n${expected}\nActual:\n${actual}")
    endif()
endfunction()
run_cli([=[set name "Ankit Kumar"
GET name
SET empty ""
GET empty
SET binary "a\0b\n"
GET binary
SET broken "unterminated
EXISTS broken
SET name
GET name
BOGUS
SCAN z a
EXIT extra
EXIT
]=] [=[OK
"Ankit Kumar"
OK
""
OK
"a\0b\n"
ERR unterminated quote
0
ERR wrong number of arguments; type HELP
"Ankit Kumar"
ERR unknown command; type HELP
ERR reversed scan bounds
ERR wrong number of arguments; type HELP
]=])
# A separate process verifies WAL recovery after EXIT without an explicit flush.
run_cli([=[GET name
EXISTS empty
DEL binary
DEL binary
GET binary
FLUSH
COMPACT
SCAN empty z
QUIT
]=] [=["Ankit Kumar"
1
1
0
(nil)
OK
OK
"empty" => ""
"name" => "Ankit Kumar"
]=])
# EOF also closes cleanly, and a deletion stays deleted after reopen.
run_cli("GET binary\nGET name\n" "(nil)\n\"Ankit Kumar\"\n")
file(REMOVE_RECURSE "${root}")
