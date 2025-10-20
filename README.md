🎯 Consistency Trainer - BakkesMod Plugin

This plugin is a consistency training tracker designed to help you accurately measure and improve your performance across various custom shot packs. It focuses on sustained consistency and boost efficiency over long practice sessions.

✨ Features

The Consistency Trainer offers granular tracking for every shot in every training pack, with all records persisting across game restarts.

Core Tracking

Session Tracking: Tracks the current attempts, successes, and consistency ratio for the active shot in real-time.

Sustained Consistency Metric: The "Lifetime Best" only updates under the following strict condition (designed to favor endurance and reliability):

The new session's success ratio must be equal to or better than the current lifetime record.

The new session's attempt count must be greater than or equal to the recorded best attempt count.

Example: A record of 5/5 attempts will only be beaten by 6/6, 7/7, or 5/5 at a lower boost cost, but not by a shorter run like 4/4.

Minimum Boost Tracking: Stores the absolute lowest amount of boost ever successfully used to complete the shot (lifetime_min_boost).

Efficiency & Boost Metrics

Current Boost: Tracks boost consumed during the currently live attempt (current_attempt_boost_used_).

Average Boost (All): Average boost used across all attempts in the current session.

Average Boost (Success/Best): Comparison of average boost used in successful shots in the current session versus the average boost used during the Lifetime Best Consistency run.

Flow Control

Max Attempts Auto-Reset: When the Max Attempts Per Shot limit is reached, the plugin automatically saves the lifetime data, resets the session stats (attempts go back to 0), and repeats the current shot, allowing for continuous cycles of consistency training without manual resets.

Robust Event Handling: Uses event handlers for Goal Scored, Shot Reset (user or plugin initiated), and Ball Exploded to guarantee every outcome is correctly recorded as a success or failure.

⚠️ Known Issues

While the plugin is designed to save your lifetime statistics, there is a current, known bug where persistent data is not reliably loading across Rocket League reboots or plugin unload/load cycles.

Your consistency session data will be tracked and saved while the game is running, but it may be lost upon restarting Rocket League. This issue is actively being investigated.

⚙️ Installation

Download: Download the latest ConsistencyTrainer.dll file.

Locate Plugin Folder: Open BakkesMod (usually F2 in-game) and go to Plugins > Plugin Manager > Open Plugin Folder.

Place DLL: Drag and drop ConsistencyTrainer.dll into the opened plugins folder.

Reload Plugins: In the BakkesMod console (F6), type plugin load consistencytrainer.

The plugin is now active. You can manage settings via F2 -> Plugins -> ConsistencyTrainer.

💻 Technical Implementation Notes

Persistence (Serialization)

Lifetime statistics are persisted across game restarts using a hidden BakkesMod CVar (ct_persistent_data). The data is serialized into a pipe-and-semicolon delimited string, ensuring backward compatibility for future updates.

Data Structure (7 Segments per Record):
PackID|ShotIndex|LBS|LBA|LBTB|LBTSB|LMB;...

Abbreviation

Description

Source

LBS

Lifetime Best Successes

ShotStats::lifetime_best_successes

LBA

Lifetime Attempts at Best

ShotStats::lifetime_attempts_at_best

LBTB

Lifetime Total Boost at Best

ShotStats::lifetime_total_boost_at_best

LBTSB

Lifetime Successful Boost at Best

ShotStats::lifetime_total_successful_boost_at_best

LMB

Lifetime Minimum Boost

ShotStats::lifetime_min_boost

Event Flow (Shot Tracking)

The tracking logic uses a state machine reliant on three key functions to ensure accurate attempt counting and outcome logging:

OnShotAttempt (Start): Fired when a new shot loads. This function immediately increments the attempt counter for the current session (resolving UI lag) and resets the current_attempt_boost_used_ accumulator to zero.

OnGoalScored, OnShotReset, OnBallExploded (Outcome): These events call HandleAttempt(isSuccess). These functions are deliberately not gated by the max attempt count to ensure the final attempt's outcome and boost usage are always processed.

HandleAttempt (Processing): Records success/failure and final boost metrics. If stats.attempts == max_attempts_per_shot_, it executes the following sequence:

Calls UpdateLifetimeBest(stats).

Calls ResetCurrentShotSessionStats(stats) (setting attempts back to 0).

Schedules RepeatCurrentShot to start the new cycle. (The next OnShotAttempt then sees attempts at 0 and sets it to 1).

CVar Settings (Quick Reference)

Use the BakkesMod console (F6) to modify these settings:

ct_plugin_enabled (Default: 0): Enable/Disable the core plugin logic.

ct_max_attempts (Default: 10): Sets the length of a single consistency run before auto-reset.

ct_window_open (Default: 0): Toggles the in-game display overlay.

ct_text_x / ct_text_y (Default: 100 / 200): Position of the in-game display.