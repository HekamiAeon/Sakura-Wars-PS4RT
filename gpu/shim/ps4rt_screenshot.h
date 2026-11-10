// SPDX-License-Identifier: GPL-2.0-or-later
// F12 screenshots of the game's own image (without the overlay), written by the presenter to the
// user folder's gpu/screenshots/. The capture itself follows the Valkyria Revolution port.
#pragma once

namespace Ps4RtScreenshot {

/// Any thread: save the next presented game frame (F12).
void Request();

/// Present thread: the number of screenshots requested since the last call. Also honours the
/// automation trigger: when PS4RT_SCREENSHOT_TRIGGER names a file, each time that file exists it
/// is removed and counts as one request (scripted QA without capturing the desktop).
unsigned Consume();

} // namespace Ps4RtScreenshot
