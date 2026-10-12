# Recovered work: AMS-Reader feature set (salvaged from session logs)

Work originally produced in a previous session under `/tmp/work/` in the
ephemeral sandbox. `/tmp` is wiped on sandbox reset, so it was lost; it was
replayed from the conversation event log
(/workspace/conversations/98b3432df79246699b316a09ee117d53).

Complete standalone modules recovered:
- main/health_policy.{c,h}, main/health_task.{c,h}   (watchdog / self-heal reboot)
- main/mqtt_publish.{c,h}, main/mqtt_bridge.{c,h}, main/mqtt_client.h  (MQTT + HA discovery)
- main/boot_confirm.{c,h}, main/boot_policy.{c,h}    (post-OTA rollback confirm)
- main/recovery_button.{c,h}, main/recovery_start.{c,h}, main/recovery_button_hw.h  (BOOT-button recovery)
- test/test_health_policy.c, test/test_mqtt_bridge.c, test/test_boot_policy.c, test/test_recovery_button.c

Caveats: NOT a drop-in patch. Integration edits into main/main.c,
main/CMakeLists.txt, main/power_history.c and test/run_host_tests.sh only
replayed partially. Not compiled or tested. Raw salvage material.
