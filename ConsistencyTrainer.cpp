#include "pch.h"
#include "ConsistencyTrainer.h"
#include "imgui/imgui.h"
// This master header includes all necessary wrapper definitions, including ActorWrapper and CanvasWrapper.
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


    // Event hooks
    // *** FIX: Hook updated to match the simplified handler signature ***
    gameWrapper->HookEvent("Function TAGame.GameMetrics_TA.GoalScored",
        [this](...) { OnGoalScored(nullptr); }); // Only passing params

    gameWrapper->HookEventWithCallerPost<ActorWrapper>("Function TAGame.GameEvent_TrainingEditor_TA.OnResetShot",
        std::bind(&ConsistencyTrainer::OnShotReset, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

    gameWrapper->HookEvent("Function TAGame.Ball_TA.EventExploded", [this](...) { OnBallExploded(nullptr); });

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
    gameWrapper->UnregisterDrawables();
    // We should unhook the constant SetVehicleInput to free resources
    gameWrapper->UnhookEvent("Function TAGame.Car_TA.SetVehicleInput");
}

// Safely initializes the session map ONLY when training starts 
void ConsistencyTrainer::InitializeSessionStats()
{
    training_session_stats_.clear();
    current_shot_index_ = 0;
    current_attempt_boost_used_ = 0.0f; // Reset boost tracker
    is_new_shot_loaded_ = true; // Mark as new shot

    if (gameWrapper->IsInCustomTraining())
    {
        ServerWrapper server = gameWrapper->GetCurrentGameState();
        if (server.IsNull()) return;

        TrainingEditorWrapper training_editor(server.memory_address);
        if (training_editor.IsNull()) return;

        int total_shots = training_editor.GetTotalRounds();
        for (int i = 0; i < total_shots; ++i) {
            training_session_stats_[i] = ShotStats();
        }
        current_shot_index_ = training_editor.GetRoundNum();
    }
    cvarManager->log("Session stats initialized.");
}

// FIX: Iterate and zero out values instead of clearing the map
void ConsistencyTrainer::ResetSessionStats()
{
    // Iterate over the existing map and zero out the counts. Safe and fast.
    for (auto& pair : training_session_stats_) {
        pair.second.attempts = 0;
        pair.second.successes = 0;
        pair.second.total_boost_used = 0.0f; // Reset boost totals
        pair.second.total_successful_boost_used = 0.0f; // Reset successful boost totals
    }
    current_attempt_boost_used_ = 0.0f; // Reset boost tracker
    is_new_shot_loaded_ = true; // Mark as new shot
    cvarManager->log("Session stats reset by user action (values zeroed).");
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

// *** FIX: Changed function definition to match simplified header declaration ***
void ConsistencyTrainer::OnGoalScored(void* params)
{
    if (!is_plugin_enabled_ || !IsInValidTraining() || IsShotFrozen()) return;
    HandleAttempt(true);
}

void ConsistencyTrainer::OnShotReset(ActorWrapper caller, void* params, std::string eventName)
{
    if (!is_plugin_enabled_ || !IsInValidTraining()) return;

    if (plugin_initiated_reset_) {
        plugin_initiated_reset_ = false;
        return;
    }

    if (IsShotFrozen()) return;

    HandleAttempt(false);
}

void ConsistencyTrainer::OnBallExploded(void* params)
{
    if (!is_plugin_enabled_ || !IsInValidTraining() || IsShotFrozen()) return;
    HandleAttempt(false);
}

void ConsistencyTrainer::OnPlaylistIndexChanged(ActorWrapper caller, void* params, std::string eventName)
{
    if (!is_plugin_enabled_ || params == nullptr) return;

    PlaylistIndexParams* p = static_cast<PlaylistIndexParams*>(params);

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
            // Max attempts reached, advance to the next shot
            gameWrapper->SetTimeout([this](...) { AdvanceToNextShot(); }, 0.05f);
        }
    }
    else if (stats.attempts > max_attempts_per_shot_) {
        // If attempts count somehow surpassed max, ensure we advance to the next shot
        gameWrapper->SetTimeout([this](...) { AdvanceToNextShot(); }, 0.05f);
    }
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
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Overall Session Stats:");
    if (training_session_stats_.empty()) { ImGui::Text("Load a training pack to see shot list."); }
    else
    {
        ImGui::Columns(6, "session_stats_table", true); // EXPANDED COLUMNS
        ImGui::Text("Shot #"); ImGui::NextColumn();
        ImGui::Text("Successes"); ImGui::NextColumn();
        ImGui::Text("Attempts"); ImGui::NextColumn();
        ImGui::Text("Consistency"); ImGui::NextColumn();
        ImGui::Text("Avg Boost (All)"); ImGui::NextColumn();
        ImGui::Text("Avg Boost (Success)"); ImGui::NextColumn();
        ImGui::Separator();
        for (const auto& pair : training_session_stats_)
        {
            float consistency = (pair.second.attempts > 0) ? (static_cast<float>(pair.second.successes) / pair.second.attempts * 100.0f) : 0.0f;
            float avg_boost = (pair.second.attempts > 0) ? (pair.second.total_boost_used / pair.second.attempts) : 0.0f;
            float avg_success_boost = (pair.second.successes > 0) ? (pair.second.total_successful_boost_used / pair.second.successes) : 0.0f;

            ImGui::Text("%d", pair.first + 1); ImGui::NextColumn();
            ImGui::Text("%d", pair.second.successes); ImGui::NextColumn();
            ImGui::Text("%d", pair.second.attempts); ImGui::NextColumn();
            ImGui::Text("%.1f%%", consistency); ImGui::NextColumn();
            ImGui::Text("%.1f", avg_boost); ImGui::NextColumn();
            ImGui::Text("%.1f", avg_success_boost); ImGui::NextColumn();
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

    // Line drawing variables
    float line_height = 20.0f * text_scale_; // Rough estimate of line height based on scale
    int current_y = text_pos_y_;
    char buffer[128];

    canvas.SetColor(255, 255, 255, 255);

    // --- 1. Consistency Stats ---
    if (show_consistency_stats_) {
        // Line 1: Current Shot
        snprintf(buffer, sizeof(buffer), "Current Shot: %d", current_shot_index_ + 1);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        // Line 2: Attempts
        snprintf(buffer, sizeof(buffer), "Attempts: %d/%d", current_stats.attempts, max_attempts_per_shot_);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        // Line 3: Successes
        snprintf(buffer, sizeof(buffer), "Successes: %d", current_stats.successes);
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

        // Line 7: Avg Success
        snprintf(buffer, sizeof(buffer), "Avg (Success): %.1f", avg_success_boost);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        // Line 8: Current Boost
        snprintf(buffer, sizeof(buffer), "Boost Current: %.1f", current_attempt_boost_used_);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
    }
}