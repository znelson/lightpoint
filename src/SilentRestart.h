#pragma once

// esp_restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to the home screen. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();
