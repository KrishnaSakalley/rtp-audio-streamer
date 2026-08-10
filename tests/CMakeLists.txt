# Test targets are added phase by phase as each component lands.
# enable_testing() lives in the root CMakeLists.txt.

add_executable(wav_test wav_test.cpp)
target_link_libraries(wav_test PRIVATE rtp_wav rtp_core)
add_test(NAME wav_round_trip COMMAND wav_test)

add_test(NAME udp_loopback_bit_identical
         COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/loopback_test.sh
                 $<TARGET_FILE:gen_test_tone> $<TARGET_FILE:rtp_sender> $<TARGET_FILE:rtp_receiver>)
set_tests_properties(udp_loopback_bit_identical PROPERTIES TIMEOUT 30)
