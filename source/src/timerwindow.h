#pragma once
// Small always-on-top desktop window showing the log clock (seconds since the mod started, the
// same time as every b2svr.log line), so a screen recording can tell when something happened.
// b2svr.ini timer_window=on (default off). It never takes focus from the game.
namespace b2s::timerwindow {

void Start();

}  // namespace b2s::timerwindow
