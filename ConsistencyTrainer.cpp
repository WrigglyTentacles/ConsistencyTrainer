#include "pch.h"
#include "ConsistencyTrainer.h"

BAKKESMOD_PLUGIN(ConsistencyTrainer, "Consistency Trainer", "1.0.0", PLUGINTYPE_CUSTOM_TRAINING)

#include "imgui/imgui.h"

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

// Called when the plugin is first loaded
void ConsistencyTrainer::onLoad()
{
	_globalCvarManager = cvarManager;

	// Register CVars (Console Variables) to store settings persistently
	cvarManager->registerCvar("ct_plugin_enabled", "0", "Enable/Disable the Consistency Trainer plugin")
		.addOnValueChanged([this](std::string, CVarWrapper cvar) {
		is_plugin_enabled_ = cvar.getBoolValue();
	});
	cvarManager->registerCvar("ct_max_attempts", "10", "Max attempts per shot for consistency tracking")
		.addOnValueChanged([this](std::string, CVarWrapper cvar) {
		max_attempts_per_shot_ = cvar.getIntValue();
	});

	// Load initial values from the registered cvars
	is_plugin_enabled_ = cvarManager->getCvar("ct_plugin_enabled").getBoolValue();
	max_attempts_per_shot_ = cvarManager->getCvar("ct_max_attempts").getIntValue();

	// Hook into game events to monitor player actions
	// Using lambdas to easily pass `this` instance to the member functions
	gameWrapper->HookEvent("Function TAGame.GameEvent_Soccar_TA.OnGoalScored", [this](...) {
		OnGoalScored(nullptr);
	});
	gameWrapper->HookEvent("Function TAGame.TrainingEditor_TA.ResetShot", [this](...) {
		OnShotReset(nullptr);
	});
	gameWrapper->HookEvent("Function TAGame.GameEvent_TrainingEditor_TA.OnShotChanged", [this](...) {
		OnTrainingShotChanged(nullptr);
	});
	gameWrapper->HookEvent("Function TAGame.GameEvent_TrainingEditor_TA.OnTrainingStarted", [this](...) {
		OnTrainingStarted(nullptr);
	});
}

// Called when the plugin is unloaded
void ConsistencyTrainer::onUnload()
{
}

// Resets all stats for the current training session
void ConsistencyTrainer::ResetSessionStats()
{
	training_session_stats_.clear();
	// Set the shot index to the current one to start tracking from here
	if (gameWrapper->IsInCustomTraining())
	{
		ServerWrapper server = gameWrapper->GetCurrentGameState();
		if (!server.IsNull()) {
			// The ServerWrapper for a custom training session can be treated as a TrainingEditorWrapper.
			TrainingEditorWrapper training_editor(server.memory_address);
			if (!training_editor.IsNull())
			{
				current_shot_index_ = training_editor.GetRoundNum();
			}
		}
	}
}

// --- Event Handlers ---

// Called when a new training pack is loaded/started
void ConsistencyTrainer::OnTrainingStarted(void* params)
{
	ResetSessionStats();
}

// Called when the player scores a goal
void ConsistencyTrainer::OnGoalScored(void* params)
{
	if (!is_plugin_enabled_ || !IsInValidTraining()) {
		return;
	}
	HandleAttemptFinished(true);
}

// Called when the player resets the shot (e.g., by pressing the reset button)
void ConsistencyTrainer::OnShotReset(void* params)
{
	if (!is_plugin_enabled_ || !IsInValidTraining()) {
		return;
	}
	// A manual reset is considered a failed attempt
	HandleAttemptFinished(false);
}

// Called when the current shot changes (either by advancing or manual selection)
void ConsistencyTrainer::OnTrainingShotChanged(void* params)
{
	if (IsInValidTraining()) {
		// Do not reset stats here, as the user might be intentionally switching shots.
		// Instead, just update the current shot index.
		ServerWrapper server = gameWrapper->GetCurrentGameState();
		if (!server.IsNull()) {
			TrainingEditorWrapper training_editor(server.memory_address);
			if (!training_editor.IsNull()) {
				current_shot_index_ = training_editor.GetRoundNum();
			}
		}
	}
}


// --- Core Logic ---

// Determines if the plugin's logic should be active
bool ConsistencyTrainer::IsInValidTraining()
{
	return gameWrapper->IsInCustomTraining();
}

// Central logic for when an attempt is completed
void ConsistencyTrainer::HandleAttemptFinished(bool isSuccess)
{
	ShotStats& stats = training_session_stats_[current_shot_index_];
	stats.attempts++;
	if (isSuccess)
	{
		stats.successes++;
	}

	if (stats.attempts >= max_attempts_per_shot_)
	{
		// If max attempts reached, move to the next shot after a short delay
		gameWrapper->SetTimeout([this](...) {
			AdvanceToNextShot();
		}, 0.1f);
	}
	else if (isSuccess) // Only reset the shot if it was a success.
	{
		// If a goal was scored and more attempts remain, explicitly reset the shot to repeat it.
		// A small delay prevents conflicts with the game's own goal scoring logic.
		gameWrapper->SetTimeout([this](...) {
			RepeatCurrentShot();
		}, 0.1f);
	}
	// If it was a fail (isSuccess == false), we don't need to do anything.
	// The user's manual reset has already put the game in the correct state.
}

void ConsistencyTrainer::AdvanceToNextShot()
{
	cvarManager->executeCommand("training_next");
}

void ConsistencyTrainer::RepeatCurrentShot()
{
	cvarManager->executeCommand("training_reset");
}


// --- ImGui Rendering ---

void ConsistencyTrainer::SetImGuiContext(uintptr_t ctx)
{
	ImGui::SetCurrentContext(reinterpret_cast<ImGuiContext*>(ctx));
}

// Main settings window (for the F2 plugins menu)
void ConsistencyTrainer::RenderSettings()
{
	if (ImGui::Checkbox("Enable Plugin", &is_plugin_enabled_))
	{
		cvarManager->getCvar("ct_plugin_enabled").setValue(is_plugin_enabled_);
	}

	ImGui::Spacing();
	ImGui::Text("Set the number of times to repeat each shot:");
	if (ImGui::SliderInt("Max Attempts", &max_attempts_per_shot_, 1, 50))
	{
		cvarManager->getCvar("ct_max_attempts").setValue(max_attempts_per_shot_);
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	if (ImGui::Button("Reset Current Session Stats"))
	{
		ResetSessionStats();
	}

	ImGui::Separator();
	ImGui::Spacing();

	// -- STATS DISPLAY --
	if (!IsInValidTraining()) {
		ImGui::Text("Load a custom training pack to see stats.");
	}
	else {
		// If no attempts have been made yet in the session, show a helpful message.
		if (training_session_stats_.empty())
		{
			ImGui::Text("Make an attempt (score or reset) to start tracking stats.");
		}
		else
		{
			ImGui::Text("Current Shot: %d", current_shot_index_ + 1); // Display 1-based index

			ShotStats& current_stats = training_session_stats_[current_shot_index_];
			float consistency = (current_stats.attempts > 0) ? (static_cast<float>(current_stats.successes) / current_stats.attempts * 100.0f) : 0.0f;

			ImGui::Text("Attempts: %d / %d", current_stats.attempts, max_attempts_per_shot_);
			ImGui::Text("Successes: %d", current_stats.successes);
			ImGui::Text("Consistency: %.1f%%", consistency);
			ImGui::ProgressBar(static_cast<float>(current_stats.attempts) / max_attempts_per_shot_);

			ImGui::Separator();
			ImGui::Text("Overall Session Stats:");

			// Use the older "Columns" API for compatibility
			ImGui::Columns(4, "shot_stats_table", true);
			ImGui::Text("Shot #"); ImGui::NextColumn();
			ImGui::Text("Successes"); ImGui::NextColumn();
			ImGui::Text("Attempts"); ImGui::NextColumn();
			ImGui::Text("Consistency"); ImGui::NextColumn();
			ImGui::Separator();

			// Iterate through all tracked shots and display their stats
			for (const auto& pair : training_session_stats_)
			{
				ImGui::Text("%d", pair.first + 1);
				ImGui::NextColumn();

				ImGui::Text("%d", pair.second.successes);
				ImGui::NextColumn();

				ImGui::Text("%d", pair.second.attempts);
				ImGui::NextColumn();

				float shot_consistency = (pair.second.attempts > 0) ? (static_cast<float>(pair.second.successes) / pair.second.attempts * 100.0f) : 0.0f;
				ImGui::Text("%.1f%%", shot_consistency);
				ImGui::NextColumn();
			}

			ImGui::Columns(1); // Important: reset to a single column
		}
	}
}

// This function is no longer used as we are not showing a separate window.
void ConsistencyTrainer::Render()
{
}


// -- Functions to satisfy the PluginWindow interface --
// These are kept to satisfy the class inheritance but are now disabled.

std::string ConsistencyTrainer::GetPluginName()
{
	return "ConsistencyTrainer";
}

std::string ConsistencyTrainer::GetMenuName()
{
	return "ConsistencyTrainer";
}

std::string ConsistencyTrainer::GetMenuTitle()
{
	return "Consistency Trainer Stats";
}

void ConsistencyTrainer::OnOpen()
{
	is_window_open_ = false;
}

void ConsistencyTrainer::OnClose()
{
	is_window_open_ = false;
}

bool ConsistencyTrainer::IsActiveOverlay()
{
	// The separate window is never active.
	return false;
}

bool ConsistencyTrainer::ShouldBlockInput()
{
	// The separate window should never block input.
	return false;
}

