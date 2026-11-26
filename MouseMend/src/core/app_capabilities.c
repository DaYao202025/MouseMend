#include "app_capabilities.h"

void app_capabilities_evaluate(const CapabilityInputs *in,
							   CapabilityState out[CAP_COUNT])
{
	if (!out)
		return;

	for (int i = 0; i < CAP_COUNT; i++)
		out[i] = CAP_STATE_DISABLED;

	if (!in)
		return;

	// The hook is never "disabled": the program always tries to install it, so
	// anything short of working is a fault the user needs to see.
	out[CAP_HOOK] = (in->hook_ready && in->hook_installed && in->hook_healthy)
						? CAP_STATE_OK
						: CAP_STATE_STALLED;

	if (in->learning_enabled)
	{
		// Started but no samples yet is the normal first minutes of use, not a
		// fault - it has to read differently from "the worker never came up".
		if (!in->learning_started)
			out[CAP_LEARNING] = CAP_STATE_STALLED;
		else if (in->learning_has_samples)
			out[CAP_LEARNING] = CAP_STATE_OK;
		else
			out[CAP_LEARNING] = CAP_STATE_UNKNOWN;
	}

	if (in->context_active)
	{
		// Paused counts as stalled on purpose: while paused the lists really do
		// nothing, and a user checking "why is the list not working" has to see
		// that rather than a reassuring green light.
		if (!in->context_started || in->context_paused)
			out[CAP_CONTEXT] = CAP_STATE_STALLED;
		else if (in->context_observed)
			out[CAP_CONTEXT] = CAP_STATE_OK;
		else
			out[CAP_CONTEXT] = CAP_STATE_UNKNOWN;
	}

	if (in->log_enabled)
	{
		// The directory is resolved lazily on the first flush, so "not yet" is
		// a normal state during the first minutes and must not look broken.
		if (!in->log_ready)
			out[CAP_LOG] = CAP_STATE_STALLED;
		else if (in->log_directory_resolved)
			out[CAP_LOG] = CAP_STATE_OK;
		else
			out[CAP_LOG] = CAP_STATE_UNKNOWN;
	}
}
