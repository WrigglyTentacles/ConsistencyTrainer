#include "pch.h"
#include "ConsistencyTrainer.h"
#include "imgui/imgui.h"
#include <limits>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <fstream> 

// Helper functions for basic serialization (using a simple, parsable format)

/**
 * @brief Serializes the persistent data into a string format.
 * * New structure (7 segments): PackID|ShotIndex|LBS|LBA|LBTB|LBTSB|LMB;...
 * LBS: lifetime_best_successes
 * LBA: lifetime_attempts_at_best (NEWLY ADDED)
 * LBTB: lifetime_total_boost_at_best
 * LBTSB: lifetime_total_successful_boost_at_best
 * LMB: lifetime_min_boost
 */
std::string SerializeStats(const PersistentData& data) {
    std::stringstream ss;
    for (const auto& pack_pair : data) {
        const std::string& pack_id = pack_pair.first;
        for (const auto& shot_pair : pack_pair.second) {
            ss << pack_id << "|"
                << shot_pair.first << "|"
                << shot_pair.second.lifetime_best_successes << "|"
                << shot_pair.second.lifetime_attempts_at_best << "|" // <-- Persist the attempts count
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

        // --- NEW 7-segment format handling ---
        // (PackID, ShotIndex, LBS, LBA, LBTB, LBTSB, LMB)
        if (segments.size() == 7) {
            std::string pack_id = segments[0];
            int shot_index = std::stoi(segments[1]);
            ShotStats s;
            s.lifetime_best_successes = std::stoi(segments[2]);
            s.lifetime_attempts_at_best = std::stoi(segments[3]); // <-- Read attempts count
            s.lifetime_total_boost_at_best = std::stof(segments[4]);
            s.lifetime_total_successful_boost_at_best = std::stof(segments[5]);
            s.lifetime_min_boost = std::stof(segments[6]);

            // Re-initialize session-specific parts safely
            s.attempts = 0; s.successes = 0; s.total_boost_used = 0.0f;
            s.total_successful_boost_used = 0.0f;
            s.min_successful_boost_used = std::numeric_limits<float>::max();

            data[pack_id][shot_index] = s;
        }
        // --- Legacy 6-segment format handling (Backward Compatibility) ---
        // (PackID, ShotIndex, LBS, LBTB, LBTSB, LMB)
        else if (segments.size() == 6) {
            std::string pack_id = segments[0];
            int shot_index = std::stoi(segments[1]);
            ShotStats s;
            s.lifetime_best_successes = std::stoi(segments[2]);
            // NOTE: For legacy data, we must assume the default max attempts (10)
            s.lifetime_attempts_at_best = 10;
            s.lifetime_total_boost_at_best = std::stof(segments[3]);
            s.lifetime_total_successful_boost_at_best = std::stof(segments[4]);
            s.lifetime_min_boost = std::stof(segments[5]);

            // Re-initialize session-specific parts safely
            s.attempts = 0; s.successes = 0; s.total_boost_used = 0.0f;
            s.total_successful_boost_used = 0.0f;
            s.min_successful_boost_used = std::numeric_limits<float>::max();

            // Log that we encountered legacy data for debugging
            if (_globalCvarManager) {
                _globalCvarManager->log("Deserialized legacy 6-segment data. Assuming lifetime_attempts_at_best = 10.");
            }

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

    // ?? CRITICAL FILE PATH FIX: Use .string() or concatenation to correctly build the absolute path.
    // This avoids the "no member toString" error and uses the BakkesMod path to ensure we can write the file.
    dataFilePath_ = gameWrapper->GetBakkesModPath().string() + "\\data\\ConsistencyTrainer.data";
    cvarManager->log("Persistence file path set to: " + dataFilePath_);

    // Register all standard CVars (unrelated to persistence string storage)
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
    cvarManager->registerCvar("ct_window_open", "0", "Show/Hide the in-game stats window")
        .addOnValueChanged([this](std::string, CVarWrapper cvar) { is_window_open_ = cvar.getBoolValue(); });
    cvarManager->registerCvar("ct_show_consistency", "1", "Show core consistency stats (attempts/successes)")
        .addOnValueChanged([this](std::string, CVarWrapper cvar) { show_consistency_stats_ = cvar.getBoolValue(); });
    cvarManager->registerCvar("ct_show_boost", "0", "Show boost usage stats")
        .addOnValueChanged([this](std::string, CVarWrapper cvar) { show_boost_stats_ = cvar.getBoolValue(); });

    // Load all current CVar settings
    is_plugin_enabled_ = cvarManager->getCvar("ct_plugin_enabled").getBoolValue();
    max_attempts_per_shot_ = cvarManager->getCvar("ct_max_attempts").getIntValue();
    text_pos_x_ = cvarManager->getCvar("ct_text_x").getIntValue();
    text_pos_y_ = cvarManager->getCvar("ct_text_y").getIntValue();
    text_scale_ = cvarManager->getCvar("ct_text_scale").getFloatValue();
    is_window_open_ = cvarManager->getCvar("ct_window_open").getBoolValue();
    show_consistency_stats_ = cvarManager->getCvar("ct_show_consistency").getBoolValue();
    show_boost_stats_ = cvarManager->getCvar("ct_show_boost").getBoolValue();

    // **Load persistent stats immediately from the dedicated file path.**
    LoadPersistentStats();

    // Event hooks
    gameWrapper->HookEvent("Function TAGame.GameMetrics_TA.GoalScored",
        [this](...) { OnGoalScored(nullptr); });

    gameWrapper->HookEventWithCallerPost<ActorWrapper>("Function TAGame.GameEvent_TrainingEditor_TA.OnResetShot",
        std::bind(&ConsistencyTrainer::OnShotReset, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

    gameWrapper->HookEvent("Function TAGame.GameEvent_TrainingEditor_TA.OnBallExploded",
        [this](...) { OnBallExploded(nullptr); });

    gameWrapper->HookEventWithCallerPost<ActorWrapper>("Function TAGame.TrainingEditorNavigation_TA.SetCurrentActivePlaylistIndex",
        std::bind(&ConsistencyTrainer::OnPlaylistIndexChanged, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

    gameWrapper->HookEvent("Function TAGame.GameEvent_TrainingEditor_TA.StartPlayTest", [this](...) { InitializeSessionStats(); });

    gameWrapper->HookEvent("Function TAGame.TrainingEditorMetrics_TA.TrainingShotAttempt",
        [this](...) { OnShotAttempt(nullptr); });

    gameWrapper->HookEvent("Function TAGame.Car_TA.SetVehicleInput", [this](std::string eventName) { OnSetVehicleInput(eventName); });


    gameWrapper->RegisterDrawable(std::bind(&ConsistencyTrainer::RenderWindow, this, std::placeholders::_1));
    cvarManager->registerNotifier("toggle_consistency_trainer", [this](...) {
        cvarManager->getCvar("ct_window_open").setValue(!is_window_open_);
    }, "Toggle the Consistency Trainer window", PERMISSION_ALL);
}

void ConsistencyTrainer::onUnload() {
    // FIX: Save current pack stats on unload
    if (!training_session_stats_.empty() && !current_pack_id_.empty()) {
        global_pack_stats_[current_pack_id_] = training_session_stats_;
    }
    // CRITICAL: Call SavePersistentStats to write data to disk when the plugin unloads.
    SavePersistentStats();

    gameWrapper->UnregisterDrawables();
    gameWrapper->UnhookEvent("Function TAGame.Car_TA.SetVehicleInput");
}

// *** FIX: Uses TrainingEditorWrapper::GetTrainingFileName() for the unique ID ***
std::string ConsistencyTrainer::GetCurrentPackID() {
    if (gameWrapper->IsInCustomTraining()) {
        ServerWrapper server = gameWrapper->GetCurrentGameState();
        if (server.IsNull()) return "";
        TrainingEditorWrapper training_editor(server.memory_address);
        if (training_editor.IsNull()) return "";
        return training_editor.GetTrainingFileName().ToString();
    }
    return "";
}

// *** NEW HELPER: Get the total number of rounds in the current pack ***
int ConsistencyTrainer::GetTotalRounds() {
    if (gameWrapper->IsInCustomTraining()) {
        ServerWrapper server = gameWrapper->GetCurrentGameState();
        if (server.IsNull()) return 0;
        TrainingEditorWrapper training_editor(server.memory_address);
        if (training_editor.IsNull()) return 0;
        return training_editor.GetTotalRounds();
    }
    return 0;
}

// ?? CRITICAL FIX: Logic to load persistent stats from a DEDICATED FILE
void ConsistencyTrainer::LoadPersistentStats() {

    // Read from the persistent data file using the stored absolute path
    std::ifstream file(dataFilePath_);
    std::string storage_str;

    if (file.is_open()) {
        std::stringstream buffer;
        buffer << file.rdbuf();
        storage_str = buffer.str();
        file.close();
        cvarManager->log("Persistence file successfully opened and read.");
    }
    else {
        global_pack_stats_.clear();
        cvarManager->log("Persistence file does not exist or could not be opened. Starting fresh.");
        return;
    }

    if (storage_str.empty()) {
        global_pack_stats_.clear();
        cvarManager->log("Persistence file was empty.");
        return;
    }

    try {
        global_pack_stats_ = DeserializeStats(storage_str);
        cvarManager->log("Loaded persistent stats for " + std::to_string(global_pack_stats_.size()) + " packs from file.");
    }
    catch (const std::exception& e) {
        cvarManager->log("Error deserializing persistent data from file. Resetting stats. Error: " + std::string(e.what()));
        global_pack_stats_.clear();
    }
}

// ?? CRITICAL FIX: Logic to save persistent stats to a DEDICATED FILE
void ConsistencyTrainer::SavePersistentStats() {
    // Only save if there is data to serialize
    if (global_pack_stats_.empty()) {
        // We do not delete the file here; we simply skip writing if the map is empty.
        cvarManager->log("No data to save. Skipping file write.");
        return;
    }

    try {
        std::string serialized_data = SerializeStats(global_pack_stats_);

        // Write the data directly to the persistent file using the stored absolute path
        std::ofstream file(dataFilePath_, std::ios::trunc); // Overwrite file
        if (file.is_open()) {
            file << serialized_data;
            file.close();
            cvarManager->log("Saved persistent stats to dedicated file.");
        }
        else {
            cvarManager->log("Error: Could not open persistence file for writing.");
        }
    }
    catch (const std::exception& e) {
        cvarManager->log("Error saving persistent data to file. Error: " + std::string(e.what()));
    }
}

// Safely initializes the session map ONLY when training starts
void ConsistencyTrainer::InitializeSessionStats()
{
    // FIX: Save existing pack stats before initializing the new pack
    // This ensures data from the last pack played is safely copied to the global persistence map *before* the session is cleared.
    if (!training_session_stats_.empty() && !current_pack_id_.empty()) {
        global_pack_stats_[current_pack_id_] = training_session_stats_;
    }
    // The SavePersistentStats() call that was previously here has been REMOVED.

    training_session_stats_.clear();
    current_shot_index_ = 0;
    current_attempt_boost_used_ = 0.0f; // Reset boost tracker
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

            // If shot is new, create the entry with defaults. This ensures training_session_stats_[i] is valid.
            if (training_session_stats_.find(i) == training_session_stats_.end()) {
                training_session_stats_[i] = ShotStats();
                // Initialize min boost to max possible float value
                training_session_stats_[i].min_successful_boost_used = std::numeric_limits<float>::max();
                training_session_stats_[i].lifetime_min_boost = std::numeric_limits<float>::max();
            }

            // CRITICAL FIX: Define the reference here to avoid scope error (E0020/C2065)
            ShotStats& stats = training_session_stats_[i];

            // For existing (and newly created) shots, reset session-specific stats
            stats.attempts = 0;
            stats.successes = 0;
            stats.total_boost_used = 0.0f;
            stats.total_successful_boost_used = 0.0f;
            stats.min_successful_boost_used = std::numeric_limits<float>::max();

            // FIX: Re-initialize the lifetime-dependent variable if missing (e.g., loaded legacy data)
            if (stats.lifetime_min_boost == 0.0f) {
                stats.lifetime_min_boost = std::numeric_limits<float>::max();
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
    cvarManager->log("Session stats reset by user action (values zeroed).");
}

// NEW HELPER: Resets only the session-specific stats for a single shot
void ConsistencyTrainer::ResetCurrentShotSessionStats(ShotStats& stats)
{
    stats.attempts = 0;
    stats.successes = 0;
    stats.total_boost_used = 0.0f;
    stats.total_successful_boost_used = 0.0f;
    stats.min_successful_boost_used = std::numeric_limits<float>::max();
    current_attempt_boost_used_ = 0.0f; // Reset global accumulator too
}

/**
 * @brief Updates the lifetime best stats based on the current session's performance.
 * * Implements logic: Update Best Attempts always, Update Success count if higher, and use boost for tie-breakers.
 */
void ConsistencyTrainer::UpdateLifetimeBest(ShotStats& stats) {

    // 1. Update Absolute Minimum Successful Boost (This is tracked independently)
    if (stats.min_successful_boost_used != std::numeric_limits<float>::max() &&
        stats.min_successful_boost_used < stats.lifetime_min_boost)
    {
        stats.lifetime_min_boost = stats.min_successful_boost_used;
        cvarManager->log("New Lifetime Best Min Boost (Individual) for Shot " + std::to_string(current_shot_index_ + 1) + ": " + std::to_string(stats.lifetime_min_boost));
    }

    // 2. Update Consistency & Associated Boost Metrics
    if (stats.attempts > 0) {

        bool attempts_increased = (stats.attempts > stats.lifetime_attempts_at_best);
        bool success_improved = (stats.successes > stats.lifetime_best_successes);

        // --- PHASE 1: Update Best Attempts (Longest Run) ---
        if (attempts_increased) {
            // Rule: Update the longest run length and record the current success/boost as the new base.
            stats.lifetime_attempts_at_best = stats.attempts;
            stats.lifetime_best_successes = stats.successes;

            // Update associated boost metrics
            stats.lifetime_total_boost_at_best = stats.total_boost_used;
            stats.lifetime_total_successful_boost_at_best = stats.total_successful_boost_used;
            cvarManager->log("New Lifetime Best Run Length (Attempts/Success) for Shot " + std::to_string(current_shot_index_ + 1) + ": " + std::to_string(stats.attempts) + "/" + std::to_string(stats.successes));
        }

        // --- PHASE 2: Update Best Success and Tie-breakers ---
        // This is checked if a longer run WASN'T just recorded, OR if the attempt length matches the current best.

        if (success_improved) {
            // Rule: Update Best Success regardless of run length (e.g., 5/7 beats 4/10).
            if (stats.successes > stats.lifetime_best_successes) {
                // If the success count is strictly higher, we take the WHOLE set of metrics for this run.
                stats.lifetime_best_successes = stats.successes;
                stats.lifetime_attempts_at_best = stats.attempts;
                stats.lifetime_total_boost_at_best = stats.total_boost_used;
                stats.lifetime_total_successful_boost_at_best = stats.total_successful_boost_used;
                cvarManager->log("New Absolute Best Success Count for Shot " + std::to_string(current_shot_index_ + 1) + ": " + std::to_string(stats.successes) + "/" + std::to_string(stats.attempts));
            }
        }
        else if (stats.successes == stats.lifetime_best_successes)
        {
            // Tie-breaker: If success count is equal, we must ensure the run length is AT LEAST equal
            // before using boost as a tie-breaker.
            if (stats.attempts >= stats.lifetime_attempts_at_best) {

                float current_total_boost = stats.total_boost_used;
                float best_total_boost = stats.lifetime_total_boost_at_best;

                // Check for strictly better boost performance
                if (current_total_boost < best_total_boost) {
                    // Update ONLY boost metrics, and set attempts_at_best to current attempts if it was previously less
                    stats.lifetime_attempts_at_best = stats.attempts;
                    stats.lifetime_total_boost_at_best = current_total_boost;
                    stats.lifetime_total_successful_boost_at_best = stats.total_successful_boost_used;
                    cvarManager->log("Boost Tie-breaker Update for Shot " + std::to_string(current_shot_index_ + 1) + ". Same success count, but less total boost used.");
                }
            }
        }
    }
}


// NEW: Gatekeeper check for when max attempts are reached
bool ConsistencyTrainer::IsShotFrozen()
{
    if (training_session_stats_.find(current_shot_index_) == training_session_stats_.end()) return true;

    ShotStats& stats = training_session_stats_.at(current_shot_index_);
    // This check is now only used internally by OnSetVehicleInput and OnShotAttempt to prevent counting/boosting
    // after the finalization logic in HandleAttempt has run.
    return stats.attempts >= max_attempts_per_shot_;
}

// FIXED: Per-tick boost tracking function using CarWrapper::GetInput()
void ConsistencyTrainer::OnSetVehicleInput(std::string eventName)
{
    // The IsShotFrozen check here is necessary to stop boost logging once the max attempts are processed.
    if (!is_plugin_enabled_ || !gameWrapper->IsInCustomTraining() || IsShotFrozen()) return;

    if (training_session_stats_.empty()) return;

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

// FIX: OnShotAttempt now increments the attempt counter for EVERY new shot, resolving visual lag.
void ConsistencyTrainer::OnShotAttempt(void* params)
{
    if (!is_plugin_enabled_ || !IsInValidTraining()) return;

    // Ensure we have stats for the current shot
    if (training_session_stats_.find(current_shot_index_) == training_session_stats_.end()) {
        training_session_stats_[current_shot_index_] = ShotStats();
    }
    ShotStats& stats = training_session_stats_.at(current_shot_index_);

    // CRITICAL FIX: Robust counter logic
    if (stats.attempts < max_attempts_per_shot_) {

        // *** Update Best Attempts (Longest Run) on attempt start ***
        // Rule: I want the best attempts to update any time the current attempts goes above the best attempts.
        if (stats.attempts + 1 > stats.lifetime_attempts_at_best) {
            stats.lifetime_attempts_at_best = stats.attempts + 1;

            // NOTE: We only update the attempts length here. The corresponding success/boost metrics will be
            // updated in UpdateLifetimeBest when the outcome is known.
            cvarManager->log("New Lifetime Best Run Length (Attempts) set to: " + std::to_string(stats.lifetime_attempts_at_best) + " upon shot start.");
        }

        // Normal increment for attempts 1 through N
        stats.attempts++;
        cvarManager->log("Shot attempt " + std::to_string(stats.attempts) + " started (Immediate Increment). Boost counter reset to 0.0.");
    }
    else if (stats.attempts >= max_attempts_per_shot_) {
        // If we are here, it means the outcome for the final attempt (N) was processed in HandleAttempt,
        // which called ResetCurrentShotSessionStats (setting attempts to 0), but the current event
        // fired before the 0 was registered, or the counter was stuck.
        // We force the reset state to 1 to cleanly start the next run.
        ResetCurrentShotSessionStats(stats); // Sets attempts to 0
        stats.attempts = 1; // Starts new run at 1
        cvarManager->log("Max attempts exceeded/Stuck counter. Forced Reset. Starting Attempt 1 of new run.");
    }

    // Reset boost accumulator for the new shot attempt
    current_attempt_boost_used_ = 0.0f;
}

// FIX: Removed IsShotFrozen() check here to allow the final attempt's outcome to be processed.
void ConsistencyTrainer::OnGoalScored(void* params)
{
    if (!is_plugin_enabled_ || !IsInValidTraining()) return;

    // Use a small timeout for success to let the game fully process the goal.
    gameWrapper->SetTimeout([this](...) {
        HandleAttempt(true);
    }, 0.05f);
}

// FIX: Removed IsShotFrozen() check here to allow the final attempt's outcome to be processed.
void ConsistencyTrainer::OnShotReset(ActorWrapper caller, void* params, std::string eventName)
{
    if (!is_plugin_enabled_ || !IsInValidTraining()) return;

    if (plugin_initiated_reset_) {
        plugin_initiated_reset_ = false;
        return;
    }

    // If the reset wasn't plugin-initiated, it was a manual user reset (which counts as a failure).
    gameWrapper->SetTimeout([this](...) {
        HandleAttempt(false);
    }, 0.05f);
}

// FIX: Removed IsShotFrozen() check here to allow the final attempt's outcome to be processed.
void ConsistencyTrainer::OnBallExploded(void* params)
{
    if (!is_plugin_enabled_ || !IsInValidTraining()) return;

    // Treat the explosion as an immediate failure and reset the shot.
    gameWrapper->SetTimeout([this](...) {
        HandleAttempt(false);
    }, 0.05f);
}

// *** CRITICAL FIX: Modified OnPlaylistIndexChanged to correct both positive and negative looping shot index ***
void ConsistencyTrainer::OnPlaylistIndexChanged(ActorWrapper caller, void* params, std::string eventName)
{
    if (!is_plugin_enabled_ || params == nullptr) return;

    PlaylistIndexParams* p = static_cast<PlaylistIndexParams*>(params);
    int new_index = p->Index;

    // Get the total number of shots in the pack
    int total_shots = GetTotalRounds();

    // 1. Correct the positive loop-back (e.g., 49 -> 50, but should be 0)
    if (total_shots > 0 && new_index >= total_shots) {
        new_index = 0;
        cvarManager->log("Playlist index looped FORWARD. Corrected index from " + std::to_string(p->Index) + " to " + std::to_string(new_index));
    }
    // 2. Correct the negative loop-back (e.g., 0 -> -1, but should be 49)
    else if (total_shots > 0 && new_index < 0) {
        new_index = total_shots - 1;
        cvarManager->log("Playlist index looped BACKWARD. Corrected index from " + std::to_string(p->Index) + " to " + std::to_string(new_index));
    }


    // If the index changed AND we were in a valid session, update the lifetime bests for the old shot.
    // This ensures min-boost and any achieved consistency ratio are saved before moving on.
    if (current_shot_index_ != new_index && !training_session_stats_.empty()) {
        // FIX: Update lifetime best *ratio* and min boost when leaving a shot
        UpdateLifetimeBest(training_session_stats_[current_shot_index_]);

        // CRITICAL: Transfer current session stats to global map.
        if (!current_pack_id_.empty()) {
            global_pack_stats_[current_pack_id_] = training_session_stats_;
        }

        // SAVE REMOVED
    }

    if (current_shot_index_ != new_index) {
        current_shot_index_ = new_index;
        cvarManager->log("Playlist index changed to: " + std::to_string(current_shot_index_));
    }
    // Reset boost tracker for the new shot
    current_attempt_boost_used_ = 0.0f;
}


// --- Core Logic ---
bool ConsistencyTrainer::IsInValidTraining() { return gameWrapper->IsInCustomTraining(); }

// MODIFIED: HandleAttempt is now the single source of truth for recording outcome and shot reset/freeze logic.
void ConsistencyTrainer::HandleAttempt(bool isSuccess)
{
    // Ensure we have stats for the current shot
    if (training_session_stats_.find(current_shot_index_) == training_session_stats_.end()) {
        // If HandleAttempt is called but no attempt event fired yet, skip.
        return;
    }

    ShotStats& stats = training_session_stats_.at(current_shot_index_);

    // Check if the current attempt number is 0. This means HandleAttempt fired but OnShotAttempt hasn't set the counter to 1 yet.
    if (stats.attempts == 0) {
        cvarManager->log("Skipping HandleAttempt: Attempt count is 0 (Race condition/Reset overlap).");
        return;
    }

    // CRITICAL FIX: Determine if this is the final attempt TO BE PROCESSED.
    bool is_final_attempt = stats.attempts == max_attempts_per_shot_;

    // 1. RECORD SUCCESS/FAILURE STATUS & BOOST USAGE FIRST
    stats.total_boost_used += current_attempt_boost_used_;

    if (isSuccess) {
        stats.successes++;
        stats.total_successful_boost_used += current_attempt_boost_used_;
        // Check and update minimum boost used on a successful shot
        if (current_attempt_boost_used_ < stats.min_successful_boost_used) {
            stats.min_successful_boost_used = current_attempt_boost_used_;
        }
        cvarManager->log("SUCCESS recorded. Attempt " + std::to_string(stats.attempts) + ". Boost Used: " + std::to_string(current_attempt_boost_used_));
    }
    else {
        cvarManager->log("FAILURE recorded. Attempt " + std::to_string(stats.attempts) + ". Boost Used: " + std::to_string(current_attempt_boost_used_));
    }

    // Reset boost tracker (OnShotAttempt will also do this, but this is safer)
    current_attempt_boost_used_ = 0.0f;

    // *** CRITICAL: Update lifetime best after EVERY attempt ***
    UpdateLifetimeBest(stats);

    // ?? CRITICAL FIX: Transfer current session stats to global map.
    // This ensures global_pack_stats_ is NOT empty and contains the latest data for serialization.
    if (!current_pack_id_.empty()) {
        global_pack_stats_[current_pack_id_] = training_session_stats_;
    }

    // SAVE REMOVED

    // 2. CHECK ATTEMPT COUNT AND DECIDE NEXT ACTION
    if (!is_final_attempt) {
        // If not the final attempt, schedule the shot reset to begin the next attempt.
        // The attempt counter for the next shot is handled by OnShotAttempt when the shot loads.
        gameWrapper->SetTimeout([this](...) {
            this->RepeatCurrentShot();
        }, 0.10f);
    }
    else {
        // Max attempts reached (e.g., attempts is 10). Auto-reset session stats and repeat shot.

        // Reset the current session stats (attempts, successes, total boost) to zero.
        // This ensures OnShotAttempt increments the counter back to 1 for the next run.
        ResetCurrentShotSessionStats(stats);

        // Then repeat the shot immediately to start the new session run
        cvarManager->log("Shot " + std::to_string(current_shot_index_ + 1) + " completed max attempts. Session stats reset and shot repeated (new run started).");
        gameWrapper->SetTimeout([this](...) { RepeatCurrentShot(); }, 0.10f);
    }
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
        ImGui::Text("Attempts/Best"); ImGui::NextColumn(); // MODIFIED HEADER
        ImGui::Text("Consistency"); ImGui::NextColumn();
        float lifetime_ratio = 0.0f;

        ImGui::Text("Avg Boost (All)"); ImGui::NextColumn();
        ImGui::Text("Avg Boost (Success/Best)"); ImGui::NextColumn(); // Combined display
        ImGui::Text("Min Boost (Curr/Best)"); ImGui::NextColumn(); // NEW COLUMN
        ImGui::Separator();
        for (const auto& pair : training_session_stats_)
        {
            // FIX: Corrected calculation for current session consistency
            float current_successes_f = static_cast<float>(pair.second.successes);
            float current_attempts_f = static_cast<float>(pair.second.attempts);
            float best_successes_f = static_cast<float>(pair.second.lifetime_best_successes);
            float best_attempts_f = static_cast<float>(pair.second.lifetime_attempts_at_best);

            float consistency = (pair.second.attempts > 0) ? (current_successes_f / current_attempts_f * 100.0f) : 0.0f;
            float best_consistency = (pair.second.lifetime_attempts_at_best > 0) ? (best_successes_f / best_attempts_f * 100.0f) : 0.0f;

            float avg_boost = (pair.second.attempts > 0) ? (pair.second.total_boost_used / pair.second.attempts) : 0.0f;
            float avg_success_boost = (pair.second.successes > 0) ? (pair.second.total_successful_boost_used / pair.second.successes) : 0.0f;
            float avg_success_boost_best_consist = (pair.second.lifetime_attempts_at_best > 0 && pair.second.lifetime_best_successes > 0) ? (pair.second.lifetime_total_successful_boost_at_best / best_successes_f) : 0.0f;
            float min_success_boost_curr = (pair.second.min_successful_boost_used != std::numeric_limits<float>::max()) ? pair.second.min_successful_boost_used : 0.0f;
            float min_success_boost_life = (pair.second.lifetime_min_boost != std::numeric_limits<float>::max()) ? pair.second.lifetime_min_boost : 0.0f;

            ImGui::Text("%d", pair.first + 1); ImGui::NextColumn();
            // Display Current/Lifetime Best Successes
            ImGui::Text("%d/%d", pair.second.successes, pair.second.lifetime_best_successes); ImGui::NextColumn();
            // Display Current/Lifetime Best Attempts
            ImGui::Text("%d/%d", pair.second.attempts, pair.second.lifetime_attempts_at_best); ImGui::NextColumn(); // MODIFIED ROW
            // Display Current Consistency / Lifetime Best Consistency
            ImGui::Text("%.1f%%/%.1f%%", consistency, best_consistency); ImGui::NextColumn(); // MODIFIED ROW
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

    // *** Allow display if a map entry exists, even if initial session stats haven't fired yet ***
    if (training_session_stats_.find(current_shot_index_) == training_session_stats_.end())
    {
        canvas.SetColor(255, 255, 255, 255);
        canvas.SetPosition(Vector2{ text_pos_x_, text_pos_y_ });
        canvas.DrawString("Loading training pack...", text_scale_, text_scale_);
        return;
    }

    ShotStats& current_stats = training_session_stats_.at(current_shot_index_);

    // Calculate values
    // Explicit casting for floating point division for consistency.
    float current_successes_f = static_cast<float>(current_stats.successes);
    float current_attempts_f = static_cast<float>(current_stats.attempts);
    float best_successes_f = static_cast<float>(current_stats.lifetime_best_successes);
    float best_attempts_f = static_cast<float>(current_stats.lifetime_attempts_at_best);

    float consistency = (current_stats.attempts > 0) ? (current_successes_f / current_attempts_f * 100.0f) : 0.0f;
    float best_consistency = (current_stats.lifetime_attempts_at_best > 0) ? (best_successes_f / best_attempts_f * 100.0f) : 0.0f;

    float avg_boost = (current_stats.attempts > 0) ? (current_stats.total_boost_used / current_stats.attempts) : 0.0f;
    float avg_success_boost = (current_stats.successes > 0) ? (current_stats.total_successful_boost_used / current_stats.successes) : 0.0f;
    float avg_success_boost_best_consist = (current_stats.lifetime_attempts_at_best > 0 && current_stats.lifetime_best_successes > 0) ? (current_stats.lifetime_total_successful_boost_at_best / best_successes_f) : 0.0f;
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

        // Line 2: Attempts (Current vs. Max/Best)
        snprintf(buffer, sizeof(buffer), "Attempts: %d/%d (Best: %d)", current_stats.attempts, max_attempts_per_shot_, current_stats.lifetime_attempts_at_best);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        // Line 3: Successes (Current vs. Lifetime Best)
        snprintf(buffer, sizeof(buffer), "Successes: %d / Best: %d", current_stats.successes, current_stats.lifetime_best_successes);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        // Line 4: Consistency (Current vs. Lifetime Best)
        snprintf(buffer, sizeof(buffer), "Consistency: %.1f%% (Best: %.1f%%)", consistency, best_consistency);
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

        // Line 9: Current Boost (This will now track accurately from OnShotAttempt reset)
        snprintf(buffer, sizeof(buffer), "Boost Current: %.1f", current_attempt_boost_used_);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
    }
}
