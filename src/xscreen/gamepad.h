// gamepad: Touch controllers (OpenXR actions) -> virtual Xbox 360 pad (uinput), see gamepad.c.
#pragma once

#include <stdbool.h>
#include <openxr/openxr.h>

//! Call before xrBeginSession (action sets are attached to the session).
bool gamepad_init(XrInstance instance, XrSession session);
//! Once per frame; focused = session state is FOCUSED (input is only delivered then).
void gamepad_update(XrSession session, bool focused);
void gamepad_close(void);
