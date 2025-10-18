#pragma once

#include "pch.h"

#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/plugin/pluginwindow.h"
#include "bakkesmod/plugin/PluginSettingsWindow.h"

#include "version.h"
constexpr auto plugin_version = stringify(VERSION_MAJOR) "." stringify(VERSION_MINOR) "." stringify(VERSION_PATCH) "." stringify(VERSION_BUILD);

// Struct to hold statistics for a single shot
struct ShotStats
{
	int attempts = 0;
	int successes = 0;
};

class ConsistencyTrainer : public BakkesMod::Plugin::BakkesModPlugin, public BakkesMod::Plugin::PluginSettingsWindow, public BakkesMod::Plugin::PluginWindow
{
public:
	// Overrides from BakkesModPlugin
	void onLoad() override;
	void onUnload() override;
	std::string GetPluginName() override;

	// Overrides from PluginSettingsWindow
	void RenderSettings() override;

	// Overrides from PluginWindow
	void Render() override;
	std::string GetMenuName() override;
	std::string GetMenuTitle() override;
	void SetImGuiContext(uintptr_t ctx) override;
	bool ShouldBlockInput() override;
	bool IsActiveOverlay() override;
	void OnOpen() override;
	void OnClose() override;

private:
	// Game event hooks
	void OnGoalScored(void* params);
	void OnShotReset(void* params);
	void OnTrainingShotChanged(void* params);
	void OnTrainingStarted(void* params);

	// Core logic
	void HandleAttemptFinished(bool isSuccess);
	void AdvanceToNextShot();
	void RepeatCurrentShot();
	void ResetSessionStats();
	bool IsInValidTraining();

	// Plugin state
	bool is_plugin_enabled_ = false;
	int max_attempts_per_shot_ = 10;
	int current_shot_index_ = 0;

	// Map to store stats for each shot index
	std::map<int, ShotStats> training_session_stats_;

	// GUI Window state
	bool is_window_open_ = false;
};

