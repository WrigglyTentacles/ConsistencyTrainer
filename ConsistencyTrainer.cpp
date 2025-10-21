#include "pch.h"
#include "ConsistencyTrainer.h"
#include "imgui/imgui.h"
#include <limits>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <map>

// Type definitions to resolve E0020
using PackStats = std::map<int, ShotStats>;
using PersistentData = std::map<std::string, PackStats>;

std::string SerializeStats(const PersistentData& data) {
    std::stringstream ss;
    for (const auto& pack_pair : data) {
        for (const auto& shot_pair : pack_pair.second) {
            ss << pack_pair.first << "|"
                << shot_pair.first << "|"
                << shot_pair.second.lifetime_best_successes << "|"
                << shot_pair.second.lifetime_attempts_at_best << "|"
                << std::to_string(shot_pair.second.lifetime_total_boost_at_best) << "|"
                << std::to_string(shot_pair.second.lifetime_total_successful_boost_at_best) << "|"
                << std::to_string(shot_pair.second.lifetime_min_boost)
                << ";";
        }
    }
    std::string result = ss.str();
    if (!result.empty()) {
        result.pop_back();
    }
    return result;
}

PersistentData DeserializeStats(const std::string& str) {
    PersistentData data;
    if (str.empty()) return data;

    std::stringstream ss(str);
    std::string record;

    while (std::getline(ss, record, ';')) {
        std::stringstream rs(record);
        std::string segment;
        std::vector<std::string> segments;

        while (std::getline(rs, segment, '|')) {
            segments.push_back(segment);
        }

        if (segments.size() == 7) {
            std::string pack_id = segments[0];
            int shot_index = std::stoi(segments[1]);
            ShotStats s;
            s.lifetime_best_successes = std::stoi(segments[2]);
            s.lifetime_attempts_at_best = std::stoi(segments[3]);
            s.lifetime_total_boost_at_best = std::stod(segments[4]);
            s.lifetime_total_successful_boost_at_best = std::stod(segments[5]);
            s.lifetime_min_boost = std::stod(segments[6]);

            s.attempts = 0; s.successes = 0; s.total_boost_used = 0.0;
            s.total_successful_boost_used = 0.0;
            s.min_successful_boost_used = std::numeric_limits<double>::max();

            data[pack_id][shot_index] = s;
        }
        else if (segments.size() == 6) {
            std::string pack_id = segments[0];
            int shot_index = std::stoi(segments[1]);
            ShotStats s;
            s.lifetime_best_successes = std::stoi(segments[2]);
            s.lifetime_attempts_at_best = 10;
            s.lifetime_total_boost_at_best = std::stod(segments[3]);
            s.lifetime_total_successful_boost_at_best = std::stod(segments[4]);
            s.lifetime_min_boost = std::stod(segments[5]);

            s.attempts = 0; s.successes = 0; s.total_boost_used = 0.0;
            s.total_successful_boost_used = 0.0;
            s.min_successful_boost_used = std::numeric_limits<double>::max();

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

struct PlaylistIndexParams {
    int Index;
};

void ConsistencyTrainer::onLoad()
{
    _globalCvarManager = cvarManager;

    dataFilePath_ = gameWrapper->GetBakkesModPath().string() + "\\data\\ConsistencyTrainer.data";
    cvarManager->log("Persistence file path set to: " + dataFilePath_);

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

    is_plugin_enabled_ = cvarManager->getCvar("ct_plugin_enabled").getBoolValue();
    max_attempts_per_shot_ = cvarManager->getCvar("ct_max_attempts").getIntValue();
    text_pos_x_ = cvarManager->getCvar("ct_text_x").getIntValue();
    text_pos_y_ = cvarManager->getCvar("ct_text_y").getIntValue();
    text_scale_ = cvarManager->getCvar("ct_text_scale").getFloatValue();
    is_window_open_ = cvarManager->getCvar("ct_window_open").getBoolValue();
    show_consistency_stats_ = cvarManager->getCvar("ct_show_consistency").getBoolValue();
    show_boost_stats_ = cvarManager->getCvar("ct_show_boost").getBoolValue();

    LoadPersistentStats();

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
    if (!training_session_stats_.empty() && !current_pack_id_.empty()) {
        global_pack_stats_[current_pack_id_] = training_session_stats_;
    }
    SavePersistentStats();

    gameWrapper->UnregisterDrawables();
    gameWrapper->UnhookEvent("Function TAGame.Car_TA.SetVehicleInput");
}

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

void ConsistencyTrainer::LoadPersistentStats() {

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

void ConsistencyTrainer::SavePersistentStats() {
    if (global_pack_stats_.empty()) {
        cvarManager->log("No data to save. Skipping file write.");
        return;
    }

    try {
        std::string serialized_data = SerializeStats(global_pack_stats_);

        std::ofstream file(dataFilePath_, std::ios::trunc);
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

void ConsistencyTrainer::ClearLifetimeStats() {
    if (current_pack_id_.empty()) {
        cvarManager->log("Cannot reset lifetime stats: No active training pack loaded.");
        return;
    }

    if (global_pack_stats_.count(current_pack_id_)) {
        global_pack_stats_.erase(current_pack_id_);
        cvarManager->log("Lifetime stats cleared for pack: " + current_pack_id_);
    }
    else {
        cvarManager->log("No saved lifetime stats found for pack: " + current_pack_id_ + ".");
    }

    training_session_stats_.clear();

    InitializeSessionStats();

    SavePersistentStats();
}


void ConsistencyTrainer::InitializeSessionStats()
{
    // Save existing pack stats before initializing the new pack
    if (!training_session_stats_.empty() && !current_pack_id_.empty()) {
        global_pack_stats_[current_pack_id_] = training_session_stats_;
    }

    // CRITICAL FIX: Clear the session map before attempting to populate it.
    training_session_stats_.clear();

    current_shot_index_ = 0;
    current_attempt_boost_used_ = 0.0;
    current_pack_id_ = GetCurrentPackID();

    if (gameWrapper->IsInCustomTraining())
    {
        ServerWrapper server = gameWrapper->GetCurrentGameState();
        if (server.IsNull()) return;

        TrainingEditorWrapper training_editor(server.memory_address);
        if (training_editor.IsNull()) return;

        // Load persistent data for the new pack if it exists
        PackStats pack_lifetime_stats;
        if (global_pack_stats_.count(current_pack_id_)) {
            pack_lifetime_stats = global_pack_stats_.at(current_pack_id_);
        }

        // Only iterate up to the total number of shots in the CURRENTLY loaded pack.
        int total_shots = training_editor.GetTotalRounds();
        for (int i = 0; i < total_shots; ++i) {

            // If lifetime data exists for this specific shot, load it into the session map.
            if (pack_lifetime_stats.count(i)) {
                training_session_stats_[i] = pack_lifetime_stats.at(i);
            }
            // If the shot is new, create the entry with defaults.
            else {
                training_session_stats_[i] = ShotStats();
            }

            // Ensure session-specific and sentinel parts are correctly initialized/reset for the current session.
            ShotStats& stats = training_session_stats_.at(i);

            // Re-initialize session-specific parts safely
            stats.attempts = 0;
            stats.successes = 0;
            stats.total_boost_used = 0.0;
            stats.total_successful_boost_used = 0.0;
            stats.min_successful_boost_used = std::numeric_limits<double>::max();

            // Check loaded lifetime_min_boost for corruption.
            if (stats.lifetime_min_boost < 1.0) {
                stats.lifetime_min_boost = std::numeric_limits<double>::max();
            }
        }
        current_shot_index_ = training_editor.GetRoundNum();
    }
    cvarManager->log("Session stats initialized for pack: " + current_pack_id_);
}

void ConsistencyTrainer::ResetSessionStats()
{
    for (auto& pair : training_session_stats_) {
        pair.second.attempts = 0;
        pair.second.successes = 0;
        pair.second.total_boost_used = 0.0;
        pair.second.total_successful_boost_used = 0.0;
        pair.second.min_successful_boost_used = std::numeric_limits<double>::max();
    }
    current_attempt_boost_used_ = 0.0;
    cvarManager->log("Session stats reset by user action (values zeroed).");
}

void ConsistencyTrainer::ResetCurrentShotSessionStats(ShotStats& stats)
{
    stats.attempts = 0;
    stats.successes = 0;
    stats.total_boost_used = 0.0;
    stats.total_successful_boost_used = 0.0;
    stats.min_successful_boost_used = std::numeric_limits<double>::max();
    current_attempt_boost_used_ = 0.0;
}

void ConsistencyTrainer::UpdateLifetimeBest(ShotStats& stats) {

    if (stats.min_successful_boost_used != std::numeric_limits<double>::max() &&
        stats.min_successful_boost_used < stats.lifetime_min_boost)
    {
        stats.lifetime_min_boost = stats.min_successful_boost_used;
        cvarManager->log("New Lifetime Best Min Boost (Individual) for Shot " + std::to_string(current_shot_index_ + 1) + ": " + std::to_string(stats.lifetime_min_boost));
    }

    if (stats.attempts > 0) {

        bool attempts_increased = (stats.attempts > stats.lifetime_attempts_at_best);
        bool success_improved = (stats.successes > stats.lifetime_best_successes);

        if (attempts_increased) {
            stats.lifetime_attempts_at_best = stats.attempts;
            stats.lifetime_best_successes = stats.successes;

            stats.lifetime_total_boost_at_best = stats.total_boost_used;
            stats.lifetime_total_successful_boost_at_best = stats.total_successful_boost_used;
            cvarManager->log("New Lifetime Best Run Length (Attempts/Success) for Shot " + std::to_string(current_shot_index_ + 1) + ": " + std::to_string(stats.attempts) + "/" + std::to_string(stats.successes));
        }

        if (success_improved) {
            if (stats.successes > stats.lifetime_best_successes) {
                stats.lifetime_best_successes = stats.successes;
                stats.lifetime_attempts_at_best = stats.attempts;
                stats.lifetime_total_boost_at_best = stats.total_boost_used;
                stats.lifetime_total_successful_boost_at_best = stats.total_successful_boost_used;
                cvarManager->log("New Absolute Best Success Count for Shot " + std::to_string(current_shot_index_ + 1) + ": " + std::to_string(stats.successes) + "/" + std::to_string(stats.attempts));
            }
        }
        else if (stats.successes == stats.lifetime_best_successes)
        {
            if (stats.attempts >= stats.lifetime_attempts_at_best) {

                double current_total_boost = stats.total_boost_used;
                double best_total_boost = stats.lifetime_total_boost_at_best;

                if (current_total_boost < best_total_boost) {
                    stats.lifetime_attempts_at_best = stats.attempts;
                    stats.lifetime_total_boost_at_best = current_total_boost;
                    stats.lifetime_total_successful_boost_at_best = stats.total_successful_boost_used;
                    cvarManager->log("Boost Tie-breaker Update for Shot " + std::to_string(current_shot_index_ + 1) + ". Same success count, but less total boost used.");
                }
            }
        }
    }
}


bool ConsistencyTrainer::IsShotFrozen()
{
    if (training_session_stats_.find(current_shot_index_) == training_session_stats_.end()) return true;

    ShotStats& stats = training_session_stats_.at(current_shot_index_);
    return stats.attempts >= max_attempts_per_shot_;
}

void ConsistencyTrainer::OnSetVehicleInput(std::string eventName)
{
    if (!is_plugin_enabled_ || !gameWrapper->IsInCustomTraining() || IsShotFrozen()) return;

    if (training_session_stats_.empty()) return;

    CarWrapper car = gameWrapper->GetLocalCar();
    if (car.IsNull()) return;

    ControllerInput input = car.GetInput();

    const double BOOST_PER_TICK = (33.333333 / 120.0);

    if (input.HoldingBoost) {
        current_attempt_boost_used_ += BOOST_PER_TICK;
    }
}


void ConsistencyTrainer::OnShotAttempt(void* params)
{
    if (!is_plugin_enabled_ || !IsInValidTraining()) return;

    if (training_session_stats_.find(current_shot_index_) == training_session_stats_.end()) {
        training_session_stats_[current_shot_index_] = ShotStats();
    }
    ShotStats& stats = training_session_stats_.at(current_shot_index_);

    if (stats.attempts < max_attempts_per_shot_) {

        if (stats.attempts + 1 > stats.lifetime_attempts_at_best) {
            stats.lifetime_attempts_at_best = stats.attempts + 1;

            cvarManager->log("New Lifetime Best Run Length (Attempts) set to: " + std::to_string(stats.lifetime_attempts_at_best) + " upon shot start.");
        }

        stats.attempts++;
        cvarManager->log("Shot attempt " + std::to_string(stats.attempts) + " started (Immediate Increment). Boost counter reset to 0.0.");
    }
    else if (stats.attempts >= max_attempts_per_shot_) {
        ResetCurrentShotSessionStats(stats);
        stats.attempts = 1;
        cvarManager->log("Max attempts exceeded/Stuck counter. Forced Reset. Starting Attempt 1 of new run.");
    }

    current_attempt_boost_used_ = 0.0;
}

void ConsistencyTrainer::OnGoalScored(void* params)
{
    if (!is_plugin_enabled_ || !IsInValidTraining()) return;

    gameWrapper->SetTimeout([this](...) {
        HandleAttempt(true);
    }, 0.05f);
}

void ConsistencyTrainer::OnShotReset(ActorWrapper caller, void* params, std::string eventName)
{
    if (!is_plugin_enabled_ || !IsInValidTraining()) return;

    if (plugin_initiated_reset_) {
        plugin_initiated_reset_ = false;
        return;
    }

    gameWrapper->SetTimeout([this](...) {
        HandleAttempt(false);
    }, 0.05f);
}

void ConsistencyTrainer::OnBallExploded(void* params)
{
    if (!is_plugin_enabled_ || !IsInValidTraining()) return;

    gameWrapper->SetTimeout([this](...) {
        HandleAttempt(false);
    }, 0.05f);
}

void ConsistencyTrainer::OnPlaylistIndexChanged(ActorWrapper caller, void* params, std::string eventName)
{
    if (!is_plugin_enabled_ || params == nullptr) return;

    PlaylistIndexParams* p = static_cast<PlaylistIndexParams*>(params);
    int new_index = p->Index;

    int total_shots = GetTotalRounds();

    if (total_shots > 0 && new_index >= total_shots) {
        new_index = 0;
        cvarManager->log("Playlist index looped FORWARD. Corrected index from " + std::to_string(p->Index) + " to " + std::to_string(new_index));
    }
    else if (total_shots > 0 && new_index < 0) {
        new_index = total_shots - 1;
        cvarManager->log("Playlist index looped BACKWARD. Corrected index from " + std::to_string(p->Index) + " to " + std::to_string(new_index));
    }


    if (current_shot_index_ != new_index && !training_session_stats_.empty()) {
        UpdateLifetimeBest(training_session_stats_[current_shot_index_]);

        if (!current_pack_id_.empty()) {
            global_pack_stats_[current_pack_id_] = training_session_stats_;
        }

        SavePersistentStats();
    }

    if (current_shot_index_ != new_index) {
        current_shot_index_ = new_index;
        cvarManager->log("Playlist index changed to: " + std::to_string(current_shot_index_));
    }
    current_attempt_boost_used_ = 0.0;
}


bool ConsistencyTrainer::IsInValidTraining() { return gameWrapper->IsInCustomTraining(); }

void ConsistencyTrainer::HandleAttempt(bool isSuccess)
{
    if (training_session_stats_.find(current_shot_index_) == training_session_stats_.end()) {
        return;
    }

    ShotStats& stats = training_session_stats_.at(current_shot_index_);

    if (stats.attempts == 0) {
        cvarManager->log("Skipping HandleAttempt: Attempt count is 0 (Race condition/Reset overlap).");
        return;
    }

    bool is_final_attempt = stats.attempts == max_attempts_per_shot_;

    stats.total_boost_used += current_attempt_boost_used_;

    if (isSuccess) {
        stats.successes++;
        stats.total_successful_boost_used += current_attempt_boost_used_;
        if (current_attempt_boost_used_ < stats.min_successful_boost_used) {
            stats.min_successful_boost_used = current_attempt_boost_used_;
        }
        cvarManager->log("SUCCESS recorded. Attempt " + std::to_string(stats.attempts) + ". Boost Used: " + std::to_string(current_attempt_boost_used_));
    }
    else {
        cvarManager->log("FAILURE recorded. Attempt " + std::to_string(stats.attempts) + ". Boost Used: " + std::to_string(current_attempt_boost_used_));
    }

    current_attempt_boost_used_ = 0.0;

    UpdateLifetimeBest(stats);

    if (!current_pack_id_.empty()) {
        global_pack_stats_[current_pack_id_] = training_session_stats_;
    }

    SavePersistentStats();

    if (!is_final_attempt) {
        gameWrapper->SetTimeout([this](...) {
            this->RepeatCurrentShot();
        }, 0.10f);
    }
    else {
        ResetCurrentShotSessionStats(stats);

        cvarManager->log("Shot " + std::to_string(current_shot_index_ + 1) + " completed max attempts. Session stats reset and shot repeated (new run started).");
        gameWrapper->SetTimeout([this](...) { RepeatCurrentShot(); }, 0.10f);
    }
}

void ConsistencyTrainer::RepeatCurrentShot()
{
    plugin_initiated_reset_ = true;
    cvarManager->executeCommand("shot_reset");
}


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
    if (ImGui::Checkbox("Show In-Game Stats", &is_window_open_)) { cvarManager->getCvar("ct_window_open").setValue(is_window_open_); }
    if (ImGui::Checkbox("Show Consistency Stats", &show_consistency_stats_)) { cvarManager->getCvar("ct_show_consistency").setValue(show_consistency_stats_); }
    if (ImGui::Checkbox("Show Boost Stats", &show_boost_stats_)) { cvarManager->getCvar("ct_show_boost").setValue(show_boost_stats_); }

    ImGui::SameLine();
    if (ImGui::Button("Reset Current Session Stats")) { ResetSessionStats(); }

    ImGui::SameLine();
    if (ImGui::Button("Reset Lifetime Stats")) { ClearLifetimeStats(); }

    ImGui::Spacing();
    if (ImGui::Button("Manually Save Lifetime Stats")) { SavePersistentStats(); }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Overall Session Stats:");
    ImGui::Text("Active Pack: %s", current_pack_id_.empty() ? "None" : current_pack_id_.c_str());
    if (training_session_stats_.empty()) { ImGui::Text("Load a training pack to see shot list."); }
    else
    {
        ImGui::Columns(7, "session_stats_table", true);
        ImGui::Text("Shot #"); ImGui::NextColumn();
        ImGui::Text("Success/Best"); ImGui::NextColumn();
        ImGui::Text("Attempts/Best"); ImGui::NextColumn();
        ImGui::Text("Consistency"); ImGui::NextColumn();
        float lifetime_ratio = 0.0f;

        ImGui::Text("Avg Boost (All)"); ImGui::NextColumn();
        ImGui::Text("Avg Boost (Success/Best)"); ImGui::NextColumn();
        ImGui::Text("Min Boost (Curr/Best)"); ImGui::NextColumn();
        ImGui::Separator();
        for (const auto& pair : training_session_stats_)
        {
            double current_successes_d = static_cast<double>(pair.second.successes);
            double current_attempts_d = static_cast<double>(pair.second.attempts);
            double best_successes_d = static_cast<double>(pair.second.lifetime_best_successes);
            double best_attempts_d = static_cast<double>(pair.second.lifetime_attempts_at_best);

            double consistency = (pair.second.attempts > 0) ? (current_successes_d / current_attempts_d * 100.0) : 0.0;
            double best_consistency = (pair.second.lifetime_attempts_at_best > 0) ? (best_successes_d / best_attempts_d * 100.0) : 0.0;

            double avg_boost = (pair.second.attempts > 0) ? (pair.second.total_boost_used / current_attempts_d) : 0.0;
            double avg_success_boost = (pair.second.successes > 0) ? (pair.second.total_successful_boost_used / current_successes_d) : 0.0;
            double avg_success_boost_best_consist = (pair.second.lifetime_attempts_at_best > 0 && pair.second.lifetime_best_successes > 0) ? (pair.second.lifetime_total_successful_boost_at_best / best_successes_d) : 0.0;

            double sentinel = std::numeric_limits<double>::max();
            double min_success_boost_curr = (pair.second.min_successful_boost_used != sentinel) ? pair.second.min_successful_boost_used : 0.0;

            // Re-implementing the display logic check for lifetime_min_boost
            // The condition is: display the actual boost ONLY if it is NOT the sentinel value.
            bool is_lifetime_boost_set = pair.second.lifetime_min_boost < 100000000.0; // Check if it's less than a huge number (safely avoiding sentinel corruption)

            double display_life_min_boost = pair.second.lifetime_min_boost;

            ImGui::Text("%d", pair.first + 1); ImGui::NextColumn();
            ImGui::Text("%d/%d", pair.second.successes, pair.second.lifetime_best_successes); ImGui::NextColumn();
            ImGui::Text("%d/%d", pair.second.attempts, pair.second.lifetime_attempts_at_best); ImGui::NextColumn();
            ImGui::Text("%.1f%%/%.1f%%", (float)consistency, (float)best_consistency); ImGui::NextColumn();
            ImGui::Text("%.1f", (float)avg_boost); ImGui::NextColumn();
            ImGui::Text("%.1f/%.1f", (float)avg_success_boost, (float)avg_success_boost_best_consist); ImGui::NextColumn();

            // FIX: Use the robust threshold check for display.
            if (is_lifetime_boost_set) {
                // If it's a reasonable, non-sentinel value, display it.
                ImGui::Text("%.1f/%.1f", (float)min_success_boost_curr, (float)display_life_min_boost); ImGui::NextColumn();
            }
            else {
                // Sentinel value or corruption detected, force print 0.0 for the 'Best' part.
                ImGui::Text("%.1f/0.0", (float)min_success_boost_curr); ImGui::NextColumn();
            }
        }
        ImGui::Columns(1);
    }
}

void ConsistencyTrainer::SetImGuiContext(uintptr_t ctx) { ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx)); }

void ConsistencyTrainer::RenderWindow(CanvasWrapper canvas)
{
    if (!is_plugin_enabled_ || !is_window_open_ || !IsInValidTraining()) { return; }

    if (training_session_stats_.find(current_shot_index_) == training_session_stats_.end())
    {
        canvas.SetColor(255, 255, 255, 255);
        canvas.SetPosition(Vector2{ text_pos_x_, text_pos_y_ });
        canvas.DrawString("Loading training pack...", text_scale_, text_scale_);
        return;
    }

    ShotStats& current_stats = training_session_stats_.at(current_shot_index_);

    double current_successes_d = static_cast<double>(current_stats.successes);
    double current_attempts_d = static_cast<double>(current_stats.attempts);
    double best_successes_d = static_cast<double>(current_stats.lifetime_best_successes);
    double best_attempts_d = static_cast<double>(current_stats.lifetime_attempts_at_best);

    double consistency = (current_stats.attempts > 0) ? (current_successes_d / current_attempts_d * 100.0) : 0.0;
    double best_consistency = (current_stats.lifetime_attempts_at_best > 0) ? (best_successes_d / best_attempts_d * 100.0) : 0.0;

    double avg_boost = (current_stats.attempts > 0) ? (current_stats.total_boost_used / current_attempts_d) : 0.0;
    double avg_success_boost = (current_stats.successes > 0) ? (current_stats.total_successful_boost_used / current_successes_d) : 0.0;
    double avg_success_boost_best_consist = (current_stats.lifetime_attempts_at_best > 0 && current_stats.lifetime_best_successes > 0) ? (current_stats.lifetime_total_successful_boost_at_best / best_successes_d) : 0.0;

    double sentinel = std::numeric_limits<double>::max();
    double min_success_boost_curr = (current_stats.min_successful_boost_used != sentinel) ? current_stats.min_successful_boost_used : 0.0;

    // Re-implementing the display logic check for lifetime_min_boost
    bool is_lifetime_boost_set = current_stats.lifetime_min_boost < 100000000.0; // Check if it's less than a huge number

    double display_life_min_boost = current_stats.lifetime_min_boost;

    float line_height = 20.0f * text_scale_;
    int current_y = text_pos_y_;
    char buffer[128];

    canvas.SetColor(255, 255, 255, 255);

    if (show_consistency_stats_) {
        snprintf(buffer, sizeof(buffer), "Current Shot: %d", current_shot_index_ + 1);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        snprintf(buffer, sizeof(buffer), "Attempts: %d/%d (Best: %d)", current_stats.attempts, max_attempts_per_shot_, current_stats.lifetime_attempts_at_best);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        snprintf(buffer, sizeof(buffer), "Successes: %d / Best: %d", current_stats.successes, current_stats.lifetime_best_successes);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        snprintf(buffer, sizeof(buffer), "Consistency: %.1f%% (Best: %.1f%%)", (float)consistency, (float)best_consistency);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height * 0.75f;
    }

    if (show_boost_stats_) {
        if (show_consistency_stats_) {
            current_y += line_height * 0.25f;
        }

        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString("--- Boost Usage ---", text_scale_, text_scale_);
        current_y += line_height;

        snprintf(buffer, sizeof(buffer), "Avg (All): %.1f", (float)avg_boost);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        snprintf(buffer, sizeof(buffer), "Avg (S) / Best (C) Avg: %.1f / %.1f", (float)avg_success_boost, (float)avg_success_boost_best_consist);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        // FIX: Use the robust threshold check for display.
        if (is_lifetime_boost_set) {
            snprintf(buffer, sizeof(buffer), "Min (S): %.1f / Best: %.1f", (float)min_success_boost_curr, (float)display_life_min_boost);
        }
        else {
            // Sentinel value or corruption detected, force print 0.0 for the 'Best' part.
            snprintf(buffer, sizeof(buffer), "Min (S): %.1f / Best: 0.0", (float)min_success_boost_curr);
        }

        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
        current_y += line_height;

        snprintf(buffer, sizeof(buffer), "Boost Current: %.1f", (float)current_attempt_boost_used_);
        canvas.SetPosition(Vector2{ text_pos_x_, current_y });
        canvas.DrawString(buffer, text_scale_, text_scale_);
    }
}