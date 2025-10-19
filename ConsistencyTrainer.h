#pragma once

#include "pch.h"
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"
// Include only the specific wrappers we need to avoid circular dependencies.
#include "bakkesmod/wrappers/CanvasWrapper.h"
#include "bakkesmod/wrappers/Engine/ActorWrapper.h"


#include <string>
#include <map>

// Struct to hold statistics for a single shot
struct ShotStats
{
	int attempts = 0;
	int successes = 0;
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
	void OnGoalScored(ActorWrapper caller, void* params, std::string eventName);
	void OnShotReset(ActorWrapper caller, void* params, std::string eventName);
	void OnBallExploded(void* params);
	void OnPlaylistIndexChanged(ActorWrapper caller, void* params, std::string eventName);
	// void OnTrainingStarted(void* params); // REMOVED from .h
	void OnShotAttempt(void* params);

	// Core logic
	void HandleAttempt(bool isSuccess);

	// Initialization (called only on training start)
	void InitializeSessionStats();
	// Reset (called by button, zeros out stats)
	void ResetSessionStats();

	bool IsInValidTraining();
	void AdvanceToNextShot();
	void RepeatCurrentShot();

	// Plugin state
	bool is_plugin_enabled_ = false;
	int max_attempts_per_shot_ = 10;
	int current_shot_index_ = 0;
	bool plugin_initiated_reset_ = false;

	// GUI Window state
	bool is_window_open_ = false;
	int text_pos_x_ = 100;
	int text_pos_y_ = 200;
	float text_scale_ = 2.0f;

	// Map to store stats for each shot index
	std::map<int, ShotStats> training_session_stats_;
};