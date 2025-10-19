#include "pch.h"
#include "ConsistencyTrainer.h"
#include "imgui/imgui.h"
#include <limits>
#include <sstream> 
#include <algorithm>
#include <iostream> 

// Helper functions for basic serialization (using a simple, parsable format)
std::string SerializeStats(const PersistentData& data) {
    std::stringstream ss;
    // Structure: PackID|ShotIndex|LBS|LBTB|LBTSB|LMB;PackID|ShotIndex|LBS|LBA|LMB;...
    for (const auto& pack_pair : data) {
        const std::string& pack_id = pack_pair.first;
        for (const auto& shot_pair : pack_pair.second) {
            ss << pack_id << "|"
                << shot_pair.first << "|"
                << shot_pair.second.lifetime_best_successes << "|"
                << shot_pair.second.lifetime_total_boost_at_best << "|"
                << shot_pair.second.lifetime_total_successful_boost_at_best << "|"
                << shot_pair.second.lifetime_min_boost
                << ";";
        }
    }
    std::string result = ss.str();
    if (!result.empty()) {
        result.pop_back(); // Remove trailing semicolon
    }
    return result;
}

PersistentData DeserializeStats(const std::string& str) {
    PersistentData data;
    if (str.empty()) return data;

    std::stringstream ss(str);
    std::string record;

    // Split by semicolon (pack records)
    while (std::getline(ss, record, ';')) {
        std::stringstream rs(record);
        std::string segment;
        std::vector<std::string> segments;

        // Split by pipe (individual fields)
        while (std::getline(rs, segment, '|')) {
            segments.push_back(segment);
        }

        // Expecting 6 segments now (PackID, ShotIndex, LBS, LBTB, LBTSB, LMB)
        if (segments.size() == 6) {
            std::string pack_id = segments[0];
            int shot_index = std::stoi(segments[1]);
            ShotStats s;
            s.lifetime_best_successes = std::stoi(segments[2]);
            s.lifetime_total_boost_at_best = std::stof(segments[3]);
            s.lifetime_total_successful_boost_at_best = std::stof(segments[4]);
            s.lifetime_min_boost = std::stof(segments[5]);

            // Re-initialize session-specific parts safely
            s.attempts = 0; s.successes = 0; s.total_boost_used = 0.0f;
            s.total_successful_boost_used = 0.0f;
            s.min_successful_boost_used = std::numeric_limits<float>::max();

            // The lifetime attempts at best should match max_attempts_per_shot_ (10 by default)
            s.lifetime_attempts_at_best = 10;

            data[pack_id][shot_index] = s;
        }
    }
    return data;
}

BAKKESMOD_PLUGIN(ConsistencyTrainer, "Consistency Trainer", "1.0.0", PLUGINTYPE_CUSTOM_TRAINING)

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

// A struct to represent the parameters passed by the SetCurrentActivePlaylistIndex event.
struct PlaylistIndexParams {
    int Index;
};

void ConsistencyTrainer::onLoad()
{
    _globalCvarManager = cvarManager;

    cvarManager->registerCvar("ct_plugin_enabled", "0", "Enable/Disable the Consistency Trainer plugin")
        .addOnValueChanged([this](std::string, CVarWrapper cvar) { is_plugin_enabled_ = cvar.getBoolValue(); });
    cvarManager->registerCvar("ct_max_attempts", "10", "Max attempts per shot for consistency tracking")
        .addOnValueChanged([this](std::string, CVarWrapper cvar) { max_attempts_per_shot_ = cvar.getIntValue(); });
    cvarManager->registerCvar("ct_text_x", "100", "X position of the stats text")
        .addOnValueChanged([this](std::string, CVarWrapper cvar) { text_pos_x_ = cvar.getIntValue(); });
    cvarManager->registerCvar("ct_text_y", "200", "Y position of the stats text")
        .addOnValueChanged([this](std::string, CVarWrapper cvar) { text_pos_y_ = cvar.getIntValue(); });
    cvarManager->registerCvar("ct_text_scale", "2.0", "Scale of the stats text")
        .addOnValueChanged([this](std::string, CVarWrapper cvar) { text_scale_ = cvar.getFloatValue(); });
    // Register CVar for Stats Window visibility
    cvarManager->registerCvar("ct_window_open", "0", "Show/Hide the in-game stats window")
        .addOnValueChanged([this](std::string, CVarWrapper cvar) { is_window_open_ = cvar.getBoolValue(); });

    // NEW: CVars for toggling stat groups
    cvarManager->registerCvar("ct_show_consistency", "1", "Show core consistency stats (attempts/successes)")
        .addOnValueChanged([this](std::string, CVarWrapper cvar) { show_consistency_stats_ = cvar.getBoolValue(); });
    cvarManager->registerCvar("ct_show_boost", "0", "Show boost usage stats")
        .addOnValueChanged([this](std::string, CVarWrapper cvar) { show_boost_stats_ = cvar.getBoolValue(); });

    // FIX: Hidden CVar for persistent string data
    cvarManager->registerCvar("ct_persistent_data", "", "String storage for lifetime stats", true, true, 0.0f, true, 0.0f, true);


    is_plugin_enabled_ = cvarManager->getCvar("ct_plugin_enabled").getBoolValue();
    max_attempts_per_shot_ = cvarManager->getCvar("ct_max_attempts").getIntValue();
    text_pos_x_ = cvarManager->getCvar("ct_text_x").getIntValue();
    text_pos_y_ = cvarManager->getCvar("ct_text_y").getIntValue();
    text_scale_ = cvarManager->getCvar("ct_text_scale").getFloatValue();
    // Load state for Stats Window
    is_window_open_ = cvarManager->getCvar("ct_window_open").getBoolValue();
    // Load state for stat toggles
    show_consistency_stats_ = cvarManager->getCvar("ct_show_consistency").getBoolValue();
    show_boost_stats_ = cvarManager->getCvar("ct_show_boost").getBoolValue();

    // FIX: Load persistent stats on plugin load
    LoadPersistentStats();


    // Event hooks
    // Hook updated to match the simplified handler signature
    gameWrapper->HookEvent("Function TAGame.GameMetrics_TA.GoalScored",
        [this](...) { OnGoalScored(nullptr); }); // Only passing params

    gameWrapper->HookEventWithCallerPost<ActorWrapper>("Function TAGame.GameEvent_TrainingEditor_TA.OnResetShot",
        std::bind(&ConsistencyTrainer::OnShotReset, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

    // OnBallExploded hook removed to prevent race condition

    gameWrapper->HookEventWithCallerPost<ActorWrapper>("Function TAGame.TrainingEditorNavigation_TA.SetCurrentActivePlaylistIndex",
        std::bind(&ConsistencyTrainer::OnPlaylistIndexChanged, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

    // Calls the robust initializer on training start
    gameWrapper->HookEvent("Function TAGame.GameEvent_TrainingEditor_TA.StartPlayTest", [this](...) { InitializeSessionStats(); });

    // Using the single-firing attempt event
    gameWrapper->HookEvent("Function TAGame.TrainingEditorMetrics_TA.TrainingShotAttempt",
        [this](...) { OnShotAttempt(nullptr); });

    // Hook: Track boost usage every tick
    gameWrapper->HookEvent("Function TAGame.Car_TA.SetVehicleInput", [this](std::string eventName) { OnSetVehicleInput(eventName); });


    gameWrapper->RegisterDrawable(std::bind(&ConsistencyTrainer::RenderWindow, this, std::placeholders::_1));
    // Notifier now toggles the CVar for the settings window
    cvarManager->registerNotifier("toggle_consistency_trainer", [this](...) {
        cvarManager->getCvar("ct_window_open").setValue(!is_window_open_);
    }, "Toggle the Consistency Trainer window", PERMISSION_ALL);
}

void ConsistencyTrainer::onUnload() {
    // FIX: Save current pack stats on unload
    if (!training_session_stats_.empty() && !current_pack_id_.empty()) {
        global_pack_stats_[current_pack_id_] = training_session_stats_;
    }
    SavePersistentStats();

    gameWrapper->UnregisterDrawables();
    // We should unhook the constant SetVehicleInput to free resources
    gameWrapper->UnhookEvent("Function TAGame.Car_TA.SetVehicleInput");
}

// *** FIX: Uses TrainingEditorWrapper::GetTrainingFileName() for the unique ID ***
std::string ConsistencyTrainer::GetCurrentPackID() {
    if (!gameWrapper->IsInCustomTraining()) return "";

    ServerWrapper server = gameWrapper->GetCurrentGameState();
    if (server.IsNull()) return "";

    TrainingEditorWrapper training_editor(server.memory_address);
    if (training_editor.IsNull()) return "";

    // Use GetTrainingFileName() as it is present in the wrapper definition provided.
    return training_editor.GetTrainingFileName().ToString();
}

// *** FIX: Logic to load persistent stats from CVar ***
void ConsistencyTrainer::LoadPersistentStats() {
    CVarWrapper cvar = cvarManager->getCvar("ct_persistent_data");
    if (!cvar) return;

    std::string storage_str = cvar.getStringValue();
    if (storage_str.empty()) {
        global_pack_stats_.clear();
        cvarManager->log("Persistence CVar is empty.");
        return;
    }

    try {
        global_pack_stats_ = DeserializeStats(storage_str);
        cvarManager->log("Loaded persistent stats for " + std::to_string(global_pack_stats_.size()) + " packs.");
    }
    catch (const std::exception& e) {
        cvarManager->log("Error deserializing persistent data. Resetting stats. Error: " + std::string(e.what()));
        global_pack_stats_.clear();
    }
}

// *** FIX: Logic to save persistent stats to CVar ***
void ConsistencyTrainer::SavePersistentStats() {
    // Only save if there is an active pack's data to save
    if (global_pack_stats_.empty()) return;

    try {
        std::string serialized_data = SerializeStats(global_pack_stats_);
        cvarManager->getCvar("ct_persistent_data").setValue(serialized_data);
        cvarManager->log("Saved persistent stats.");
    }
    catch (const std::exception& e) {
        cvarManager->log("Error saving persistent data. Error: " + std::string(e.what()));
    }
}

// Safely initializes the session map ONLY when training starts 
void ConsistencyTrainer::InitializeSessionStats()
{
    // FIX: Save existing pack stats before initializing the new pack
    if (!training_session_stats_.empty() && !current_pack_id_.empty()) {
        global_pack_stats_[current_pack_id_] = training_session_stats_;
    }

    training_session_stats_.clear();
    current_shot_index_ = 0;
    current_attempt_boost_used_ = 0.0f; // Reset boost tracker
    is_new_shot_loaded_ = true; // Mark as new shot
    current_pack_id_ = GetCurrentPackID(); // Set new pack ID

    if (gameWrapper->IsInCustomTraining())
    {
        ServerWrapper server = gameWrapper->GetCurrentGameState();
        if (server.IsNull()) return;

        TrainingEditorWrapper training_editor(server.memory_address);
        if (training_editor.IsNull()) return;

        // FIX: Load persistent data for the new pack if it exists
        if (global_pack_stats_.count(current_pack_id_)) {
            // Load lifetime stats into the session map
            training_session_stats_ = global_pack_stats_.at(current_pack_id_);
        }

        // Ensure all rounds exist and initialize any missing or session-specific parts
        int total_shots = training_editor.GetTotalRounds();
        for (int i = 0; i < total_shots; ++i) {
            if (training_session_stats_.find(i) == training_session_stats_.end()) {
                // If a shot is new or missing, create a fresh entry
                training_session_stats_[i] = ShotStats();
                // Initialize min boost to max possible float value
                training_session_stats_[i].min_successful_boost_used = std::numeric_limits<float>::max();
            }
            else {
                // For existing shots, reset session-specific stats
                ShotStats& stats = training_session_stats_[i];
                stats.attempts = 0;
                stats.successes = 0;
                stats.total_boost_used = 0.0f;
                stats.total_successful_boost_used = 0.0f;
                stats.min_successful_boost_used = std::numeric_limits<float>::max();
                // FIX: Re-initialize the lifetime-dependent variable if missing
                if (stats.lifetime_min_boost == 0.0f) {
                    stats.lifetime_min_boost = std::numeric_limits<float>::max();
                }
            }
        }
        current_shot_index_ = training_editor.GetRoundNum();
    }
    cvarManager->log("Session stats initialized for pack: " + current_pack_id_);
}

// FIX: Iterate and zero out values instead of clearing the map
void ConsistencyTrainer::ResetSessionStats()
{
    // Iterate over the existing map and zero out the current session counts. 
    for (auto& pair : training_session_stats_) {
        // Only reset session-specific values, preserve lifetime data
        pair.second.attempts = 0;
        pair.second.successes = 0;
        pair.second.total_boost_used = 0.0f;
        pair.second.total_successful_boost_used = 0.0f;
        // Reset current session min boost to max possible float value
        pair.second.min_successful_boost_used = std::numeric_limits<float>::max();
    }
    current_attempt_boost_used_ = 0.0f;
    is_new_shot_loaded_ = true;
    cvarManager->log("Session stats reset by user action (values zeroed).");
}

// Logic to check and update lifetime bests
void ConsistencyTrainer::UpdateLifetimeBest(ShotStats& stats) {
    // We only update lifetime bests when the current session's attempts equal the max.
    if (stats.attempts == max_attempts_per_shot_) {

        // 1. Update Consistency & Associated Boost Metrics
        // Check if current success count is better than the lifetime best
        if (stats.successes > stats.lifetime_best_successes) {
            stats.lifetime_best_successes = stats.successes;
            // Record the metrics associated with this new best consistency run
            stats.lifetime_total_boost_at_best = stats.total_boost_used;
            stats.lifetime_total_successful_boost_at_best = stats.total_successful_boost_used;
            // The attempts are always max_attempts_per_shot_
            stats.lifetime_attempts_at_best = max_attempts_per_shot_;

            cvarManager->log("New Lifetime Best Consistency for Shot " + std::to_string(current_shot_index_ + 1) + ": " + std::to_string(stats.successes) + "/" + std::to_string(stats.lifetime_attempts_at_best));
        }
    }

    // 2. Update Absolute Minimum Successful Boost (This is tracked independently)
    // Check if the best min_successful_boost_used recorded in the current session (for THIS specific shot) is better than the lifetime best.
    if (stats.min_successful_boost_used != std::numeric_limits<float>::max() &&
        stats.min_successful_boost_used < stats.lifetime_min_boost)
    {
        stats.lifetime_min_boost = stats.min_successful_boost_used;
        cvarManager->log("New Lifetime Best Min Boost (Individual) for Shot " + std::to_string(current_shot_index_ + 1) + ": " + std::to_string(stats.lifetime_min_boost));
    }
}


// NEW: Gatekeeper check for when max attempts are reached
bool ConsistencyTrainer::IsShotFrozen()
{
    if (training_session_stats_.find(current_shot_index_) == training_session_stats_.end()) return true;

    ShotStats& stats = training_session_stats_[current_shot_index_];
    return stats.attempts >= max_attempts_per_shot_;
}

// FIXED: Per-tick boost tracking function using CarWrapper::GetInput()
void ConsistencyTrainer::OnSetVehicleInput(std::string eventName)
{
    if (!is_plugin_enabled_ || !gameWrapper->IsInCustomTraining() || IsShotFrozen()) return;

    // Check if the current shot has started (to accumulate boost)
    if (is_new_shot_loaded_ || training_session_stats_.empty()) return;

    CarWrapper car = gameWrapper->GetLocalCar();
    if (car.IsNull()) return;

    // Get input directly from the local car wrapper
    ControllerInput input = car.GetInput();

    // Standard boost usage rate is 33.33 units per second. Physics tick is 120Hz.
    const float BOOST_PER_TICK = (33.333333f / 120.0f);

    // Check if the player is holding the boost button
    if (input.HoldingBoost) {
        current_attempt_boost_used_ += BOOST_PER_TICK;
    }
}


// --- Event Handlers ---
void ConsistencyTrainer::OnShotAttempt(void* params)
{
    if (!is_plugin_enabled_ || !IsInValidTraining() || IsShotFrozen()) return;

    // The is_new_shot_loaded_ flag ensures we only do this once after the shot starts.
    if (is_new_shot_loaded_)
    {
        if (training_session_stats_.find(current_shot_index_) != training_session_stats_.end())
        {
            ShotStats& stats = training_session_stats_[current_shot_index_];
            if (stats.attempts < max_attempts_per_shot_) {
                stats.attempts++;
                cvarManager->log("Attempt " + std::to_string(stats.attempts) + " started for shot " + std::to_string(current_shot_index_ + 1));
            }
        }
        is_new_shot_loaded_ = false;
    }
}

// FIX: Changed function definition to match simplified header declaration
void ConsistencyTrainer::OnGoalScored(void* params)
{
    if (!is_plugin_enabled_ || !IsInValidTraining() || IsShotFrozen()) return;

    // Use a small timeout for success to let the game fully process the goal and prevent the reset handler from firing immediately.
    gameWrapper->SetTimeout([this](...) {
        HandleAttempt(true);
    }, 0.01f);
}

void ConsistencyTrainer::OnShotReset(ActorWrapper caller, void* params, std::string eventName)
{
    if (!is_plugin_enabled_ || !IsInValidTraining()) return;

    if (plugin_initiated_reset_) {
        plugin_initiated_reset_ = false;
        return;
    }

    if (IsShotFrozen()) return;

    // Use a small timeout for failure too, to ensure we don't interfere with the delayed success logic.
    gameWrapper->SetTimeout([this](...) {
        HandleAttempt(false);
    }, 0.01f);
}

void ConsistencyTrainer::OnBallExploded(void* params)
{
    // Do nothing. Reliance is now on OnGoalScored and OnShotReset.
}

// *** MODIFIED: OnPlaylistIndexChanged now handles saving/updating lifetime bests on shot change ***
void ConsistencyTrainer::OnPlaylistIndexChanged(ActorWrapper caller, void* params, std::string eventName)
{
    if (!is_plugin_enabled_ || params == nullptr) return;

    PlaylistIndexParams* p = static_cast<PlaylistIndexParams*>(params);

    // If the index changed AND we were in a valid session, update the lifetime bests for the old shot.
    if (current_shot_index_ != p->Index && !training_session_stats_.empty()) {
        UpdateLifetimeBest(training_session_stats_[current_shot_index_]);
        // Also save the current session data immediately when the player moves to a different shot
        SavePersistentStats();
    }

    if (current_shot_index_ != p->Index) {
        current_shot_index_ = p->Index;
        cvarManager->log("Playlist index changed to: " + std::to_string(current_shot_index_));
    }
    // Reset boost tracker for the new shot
    current_attempt_boost_used_ = 0.0f;
    is_new_shot_loaded_ = true;
}


// --- Core Logic ---
bool ConsistencyTrainer::IsInValidTraining() { return gameWrapper->IsInCustomTraining(); }

// MODIFIED: HandleAttempt now saves boost data
void ConsistencyTrainer::HandleAttempt(bool isSuccess)
{
    // Ensure we have stats for the current shot
    if (training_session_stats_.find(current_shot_index_) == training_session_stats_.end()) {
        training_session_stats_[current_shot_index_] = ShotStats();
    }

    ShotStats& stats = training_session_stats_[current_shot_index_];

    // We only proceed if an attempt was recorded (stats.attempts > 0)
    if (stats.attempts > 0 && stats.attempts <= max_attempts_per_shot_) {

        // 1. RECORD SUCCESS/FAILURE STATUS & BOOST USAGE FIRST
        stats.total_boost_used += current_attempt_boost_used_;

        if (isSuccess) {
            stats.successes++;
            stats.total_successful_boost_used += current_attempt_boost_used_;
            // Check and update minimum boost used on a successful shot
            if (current_attempt_boost_used_ < stats.min_successful_boost_used) {
                stats.min_successful_boost_used = current_attempt_boost_used_;
            }
            cvarManager->log("SUCCESS recorded. Boost Used: " + std::to_string(current_attempt_boost_used_));
        }
        else {
            cvarManager->log("FAILURE recorded for attempt " + std::to_string(stats.attempts) + ". Boost Used: " + std::to_string(current_attempt_boost_used_));
        }

        // Reset boost tracker for the next attempt (even if it's a repeat of the same shot)
        current_attempt_boost_used_ = 0.0f;
        is_new_shot_loaded_ = true; // Set to true so OnShotAttempt can fire again

        // 2. CHECK ATTEMPT COUNT AND DECIDE NEXT ACTION
        if (stats.attempts < max_attempts_per_shot_) {
            gameWrapper->SetTimeout([this](...) { RepeatCurrentShot(); }, 0.05f);
        }
        else {
            // *** FIX: Update lifetime best on final attempt BEFORE ADVANCING ***
            UpdateLifetimeBest(stats);
            // *** FIX: AUTOMATICALLY SAVE STATS ON SHOT COMPLETION ***
            SavePersistentStats();

            // Max attempts reached, advance to the next shot
            gameWrapper->SetTimeout([this](...) { AdvanceToNextShot(); }, 0.05f);
        }
    }
    else if (stats.attempts > max_attempts_per_shot_) {
        // If attempts count somehow surpassed max, ensure we advance to the next shot
        gameWrapper->SetTimeout([this](...) { AdvanceToNextShot(); }, 0.05f);
    }
    // If stats.attempts is 0 (i.e. the array was cleared and HandleAttempt was called without OnShotAttempt), do nothing.
}

void ConsistencyTrainer::AdvanceToNextShot() {
    plugin_initiated_reset_ = true;
    cvarManager->executeCommand("training_next");
}

void ConsistencyTrainer::RepeatCurrentShot()
{
    plugin_initiated_reset_ = true;
    cvarManager->executeCommand("shot_reset");
}


// --- ImGui & Canvas Rendering ---

std::string ConsistencyTrainer::GetPluginName() { return "ConsistencyTrainer"; }

void ConsistencyTrainer::RenderSettings()
{
    if (ImGui::Checkbox("Enable Plugin", &is_plugin_enabled_)) { cvarManager->getCvar("ct_plugin_enabled").setValue(is_plugin_enabled_); }
    ImGui::Spacing();
    if (ImGui::SliderInt("Max Attempts Per Shot", &max_attempts_per_shot_, 1, 50)) { cvarManager->getCvar("ct_max_attempts").setValue(max_attempts_per_shot_); }
    ImGui::Spacing();
    ImGui::Text("Display Settings:");
    if (ImGui::SliderInt("Text X Position", &text_pos_x_, 0, 1920)) { cvarManager->getCvar("ct_text_x").setValue(text_pos_x_); }
    if (ImGui::SliderInt("Text Y Position", &text_pos_y_, 0, 1080)) { cvarManager->getCvar("ct_text_y").setValue(text_pos_y_); }
    if (ImGui::SliderFloat("Text Scale", &text_scale_, 1.0f, 10.0f)) { cvarManager->getCvar("ct_text_scale").setValue(text_scale_); }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    // Checkbox for permanent setting
    if (ImGui::Checkbox("Show In-Game Stats", &is_window_open_)) { cvarManager->getCvar("ct_window_open").setValue(is_window_open_); }
    // NEW: Consistency Stats Toggle
    if (ImGui::Checkbox("Show Consistency Stats", &show_consistency_stats_)) { cvarManager->getCvar("ct_show_consistency").setValue(show_consistency_stats_); }
    // NEW: Boost Stats Toggle
    if (ImGui::Checkbox("Show Boost Stats", &show_boost_stats_)) { cvarManager->getCvar("ct_show_boost").setValue(show_boost_stats_); }

    ImGui::SameLine();
    if (ImGui::Button("Reset Current Session Stats")) { ResetSessionStats(); }
    ImGui::Spacing();
    // NEW: Manual Save button remains for user convenience/debug
    if (ImGui::Button("Manually Save Lifetime Stats")) { SavePersistentStats(); }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Overall Session Stats:");
    ImGui::Text("Active Pack: %s", current_pack_id_.empty() ? "None" : current_pack_id_.c_str());
    if (training_session_stats_.empty()) { ImGui::Text("Load a training pack to see shot list."); }
    else
    {
        ImGui::Columns(7, "session_stats_table", true); // EXPANDED COLUMNS
        ImGui::Text("Shot #"); ImGui::NextColumn();
        ImGui::Text("Success/Best"); ImGui::NextColumn();
        ImGui::Text("Attempts"); ImGui::NextColumn();
        ImGui::Text("Consistency"); ImGui::NextColumn();
        ImGui::Text("Avg Boost (All)"); ImGui::NextColumn();
        ImGui::Text("Avg Boost (Success/Best)"); ImGui::NextColumn(); // Combined display
        ImGui::Text("Min Boost (Curr/Best)"); ImGui::NextColumn(); // NEW COLUMN
        ImGui::Separator();
        for (const auto& pair : training_session_stats_)
        {
            float consistency = (pair.second.attempts > 0) ? (static_cast<float>(pair.second.successes) / pair.second.attempts * 100.0f) : 0.0f;
            float avg_boost = (pair.second.attempts > 0) ? (pair.second.total_boost_used / pair.second.attempts) : 0.0f;
            float avg_success_boost = (pair.second.successes > 0) ? (pair.second.total_successful_boost_used / pair.second.successes) : 0.0f;
            float avg_success_boost_best_consist = (pair.second.lifetime_attempts_at_best > 0 && pair.second.lifetime_best_successes > 0) ? (pair.second.lifetime_total_successful_boost_at_best / pair.second.lifetime_best_successes) : 0.0f;

            float min_success_boost_curr = (pair.second.min_successful_boost_used != std::numeric_limits<float>::max()) ? pair.second.min_successful_boost_used : 0.0f;
            float min_success_boost_life = (pair.second.lifetime_min_boost != std::numeric_limits<float>::max()) ? pair.second.lifetime_min_boost : 0.0f;

            ImGui::Text("%d", pair.first + 1); ImGui::NextColumn();
            // Display Current/Lifetime Best Consistency
            ImGui::Text("%d/%d", pair.second.successes, pair.second.lifetime_best_successes); ImGui::NextColumn();
            ImGui::Text("%d", pair.second.attempts); ImGui::NextColumn();
            ImGui::Text("%.1f%%", consistency); ImGui::NextColumn();
            ImGui::Text("%.1f", avg_boost); ImGui::NextColumn();
            ImGui::Text("%.1f/%.1f", avg_success_boost, avg_success_boost_best_consist); ImGui::NextColumn();
            ImGui::Text("%.1f/%.1f", min_success_boost_curr, min_success_boost_life); ImGui::NextColumn();
        }
        ImGui::Columns(1);
    }
}

void ConsistencyTrainer::SetImGuiContext(uintptr_t ctx) { ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx)); }

// MODIFIED: RenderWindow now manually draws lines based on state toggles
void ConsistencyTrainer::RenderWindow(CanvasWrapper canvas)
{
    if (!is_plugin_enabled_ || !is_window_open_ || !IsInValidTraining()) { return; }

    if (training_session_stats_.empty() || training_session_stats_.find(current_shot_index_) == training_session_stats_.end())
    {
        canvas.SetColor(255, 255, 255, 255);
        canvas.SetPosition(Vector2{ text_pos_x_, text_pos_y_ });
        canvas.DrawString("Loading training pack...", text_scale_, text_scale_);
        return;
    }

    ShotStats& current_stats = training_session_stats_.at(current_shot_index_);

    // Calculate values
    float consistency = (current_stats.attempts > 0) ? (static_cast<float>(current_stats.successes) / current_stats.attempts * 100.0f) : 0.0f;
    float avg_boost = (current_stats.attempts > 0) ? (current_stats.total_boost_used / current_stats.attempts) : 0.0f;
    float avg_success_boost = (current_stats.successes > 0) ? (current_stats.total_successful_boost_used / current_stats.successes) : 0.0f;
    float avg_success_boost_best_consist = (current_stats.lifetime_attempts_at_best > 0 && current_stats.lifetime_best_successes > 0) ? (current_stats.lifetime_total_successful_boost_at_best / current_stats.lifetime_best_successes) : 0.0f;
    float min_success_boost_curr = (current_stats.min_successful_boost_used != std::numeric_limits<float>::max()) ? current_stats.min_successful_boost_used : 0.0f;
    float min_success_boost_life = (current_stats.lifetime_min_boost != std::numeric_limits<float>::max()) ? current_stats.lifetime_min_boost : 0.0f;


    // Line drawing variables
    float line_height = 20.0f * text_scale_; // Rough estimate of line height based on scale
    int current_y = text_pos_y_;
    char buffer[128];

    canvas.SetColor(255, 255, 255, 255);

    // --- 1. Consistency Stats ---
    if (show_consistency_stats_) {
        // *** FIX: Removed Pack ID from canvas display ***
        snprintf(buffer, sizeof(buffer), "Current Shot: %d", current_shot_index_ + 1);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        // Line 2: Attempts
        snprintf(buffer, sizeof(buffer), "Attempts: %d/%d", current_stats.attempts, max_attempts_per_shot_);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        // Line 3: Successes (Current vs. Lifetime Best)
        snprintf(buffer, sizeof(buffer), "Successes: %d / Best: %d", current_stats.successes, current_stats.lifetime_best_successes);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        // Line 4: Consistency
        snprintf(buffer, sizeof(buffer), "Consistency: %.1f%%", consistency);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height * 0.75f; // Add a small gap after the group
    }

    // --- 2. Boost Stats ---
    if (show_boost_stats_) {
        if (show_consistency_stats_) {
            current_y += line_height * 0.25f; // Add a small gap only if consistency stats were shown
        }

        // Line 5: Group Title
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString("--- Boost Usage ---", text_scale_, text_scale_);
        current_y += line_height;

        // Line 6: Avg All
        snprintf(buffer, sizeof(buffer), "Avg (All): %.1f", avg_boost);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        // Line 7: Avg Success / Avg Best Consistency
        snprintf(buffer, sizeof(buffer), "Avg (S) / Best (C) Avg: %.1f / %.1f", avg_success_boost, avg_success_boost_best_consist);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        // Line 8: Min Success (Current vs. Lifetime Best)
        snprintf(buffer, sizeof(buffer), "Min (S): %.1f / Best: %.1f", min_success_boost_curr, min_success_boost_life);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        // Line 9: Current Boost
        snprintf(buffer, sizeof(buffer), "Boost Current: %.1f", current_attempt_boost_used_);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
    }
}