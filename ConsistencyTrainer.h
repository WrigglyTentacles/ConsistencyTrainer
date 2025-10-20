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
#include <limits>
#include <sstream>

// Struct to hold statistics for a single shot
struct ShotStats
{
    int attempts = 0;
    int successes = 0;
    // Boost tracking variables (Current Session)
    float total_boost_used = 0.0f;
    float total_successful_boost_used = 0.0f;
    float min_successful_boost_used = std::numeric_limits<float>::max();

    // Persistent Lifetime Bests (Metrics tracked from the best consistency run)
    int lifetime_best_successes = 0;
    int lifetime_attempts_at_best = 0; // Will be equal to max_attempts_per_shot_
    float lifetime_total_boost_at_best = 0.0f;
    float lifetime_total_successful_boost_at_best = 0.0f;
    float lifetime_min_boost = std::numeric_limits<float>::max(); // Absolute lowest boost used on any successful shot
};

// This map holds all lifetime ShotStats, keyed by Shot Index (int).
using ShotPackStats = std::map<int, ShotStats>;
// This map holds all ShotPacks, keyed by the Training Pack Code (string).
using PersistentData = std::map<std::string, ShotPackStats>;


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
    // Persistence methods use string serialization
    void LoadPersistentStats();
    void SavePersistentStats();
    void UpdateLifetimeBest(ShotStats& current_stats);
    std::string GetCurrentPackID();
    // *** NEW: Helper to get the total number of shots for index correction ***
    int GetTotalRounds();

    // Game event hooks
    void OnGoalScored(void* params);
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
    // NEW: Helper to reset session stats for the current shot
    void ResetCurrentShotSessionStats(ShotStats& stats);
    bool IsInValidTraining();
    // AdvanceToNextShot() removed.
    void RepeatCurrentShot();

    // Plugin state
    bool is_plugin_enabled_ = false;
    int max_attempts_per_shot_ = 10;
    int current_shot_index_ = 0;
    bool plugin_initiated_reset_ = false;

    // Boost tracker for the current attempt
    float current_attempt_boost_used_ = 0.0f;

    // Stat Toggle States
    bool show_consistency_stats_ = true;
    bool show_boost_stats_ = false;

    // The container for ALL lifetime data
    PersistentData global_pack_stats_;
    std::string current_pack_id_ = "";

    // GUI Window state
    bool is_window_open_ = false;
    int text_pos_x_ = 100;
    int text_pos_y_ = 200;
    float text_scale_ = 2.0f;

    // Map to store stats for each shot index (Local Session)
    std::map<int, ShotStats> training_session_stats_;
};