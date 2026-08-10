# Test targets are added phase by phase as each component lands.
# enable_testing() lives in the root CMakeLists.txt.

add_executable(wav_test wav_test.cpp)
target_link_libraries(wav_test PRIVATE rtp_wav rtp_core)
add_test(NAME wav_round_trip COMMAND wav_test)

add_executable(rtp_packet_test rtp_packet_test.cpp)
target_link_libraries(rtp_packet_test PRIVATE rtp_packet rtp_core)
add_test(NAME rtp_packet_round_trip COMMAND rtp_packet_test)

add_executable(stats_test stats_test.cpp)
target_link_libraries(stats_test PRIVATE rtp_stats rtp_core)
add_test(NAME sequence_tracker_wrap COMMAND stats_test)

add_executable(codec_test codec_test.cpp)
target_link_libraries(codec_test PRIVATE rtp_codec rtp_core)
add_test(NAME codec_round_trip COMMAND codec_test)

# snr_harness itself exits nonzero if SNR falls below the Phase 3 GATE
# threshold, so no separate assertion logic is needed here.
add_test(NAME snr_ulaw_above_30db COMMAND snr_harness --codec ulaw)
add_test(NAME snr_adpcm_above_18db COMMAND snr_harness --codec adpcm)

add_test(NAME udp_loopback_bit_identical
         COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/loopback_test.sh
                 $<TARGET_FILE:gen_test_tone> $<TARGET_FILE:rtp_sender> $<TARGET_FILE:rtp_receiver>)
set_tests_properties(udp_loopback_bit_identical PROPERTIES TIMEOUT 30)
