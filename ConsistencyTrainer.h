#pragma once

#include "pch.h"
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"
// Include only the specific wrappers we need to avoid circular dependencies.
#include "bakkesmod/wrappers/CanvasWrapper.h"

// Corrected wrapper paths for necessary classes
#include "bakkesmod/wrappers/GameObject/CarWrapper.h"
#include "bakkesmod/wrappers/PlayerControllerWrapper.h" 

#include <string>
#include <map>
#include <limits> // Required for initializing min_successful_boost_used

// Struct to hold statistics for a single shot
struct ShotStats
{
	int attempts = 0;
	int successes = 0;
	// Boost tracking variables
	float total_boost_used = 0.0f;
	float total_successful_boost_used = 0.0f;
	// *** NEW: Tracks the least amount of boost used on a successful attempt ***
	float min_successful_boost_used = std::numeric_limits<float>::max();
};

class ConsistencyTrainer : public BakkesMod::Plugin::BakkesModPlugin, public BakkesMod::Plugin::PluginSettingsWindow
{
public:
	// Overrides from BakkesModPlugin
	void onLoad() override;
	void onUnload() override;

	// Overrides from PluginSettingsWindow
	std::string GetPluginName() override;
	void RenderSettings() override;
	void SetImGuiContext(uintptr_t ctx) override;

	// Our custom rendering function
	void RenderWindow(CanvasWrapper canvas);

private:
	// Game event hooks
	void OnGoalScored(void* params); // Simplified due to HookEvent usage in CPP
	void OnShotReset(ActorWrapper caller, void* params, std::string eventName);
	void OnBallExploded(void* params);
	void OnPlaylistIndexChanged(ActorWrapper caller, void* params, std::string eventName);
	void OnShotAttempt(void* params);

	// Boost usage tracking hook
	void OnSetVehicleInput(std::string eventName);


	// Core logic
	bool IsShotFrozen();
	void HandleAttempt(bool isSuccess);
	void InitializeSessionStats();
	void ResetSessionStats();
	bool IsInValidTraining();
	void AdvanceToNextShot();
	void RepeatCurrentShot();

	// Plugin state
	bool is_plugin_enabled_ = false;
	int max_attempts_per_shot_ = 10;
	int current_shot_index_ = 0;
	bool plugin_initiated_reset_ = false;

	// Boost tracker for the current attempt
	float current_attempt_boost_used_ = 0.0f;
	// Flag to track when a new shot starts (for boost reset)
	bool is_new_shot_loaded_ = true;

	// NEW: Stat Toggle States
	bool show_consistency_stats_ = true;
	bool show_boost_stats_ = false;

	// GUI Window state
	bool is_window_open_ = false;
	int text_pos_x_ = 100;
	int text_pos_y_ = 200;
	float text_scale_ = 2.0f;

	// Map to store stats for each shot index
	std::map<int, ShotStats> training_session_stats_;
};