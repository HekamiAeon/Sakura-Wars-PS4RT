// SPDX-License-Identifier: GPL-2.0-or-later
// F12 screenshots of the game's own image (without the overlay), written by the presenter to the
// user folder's gpu/screenshots/. The capture itself follows the Valkyria Revolution port.
#pragma once

namespace Ps4RtScreenshot {

/// Any thread: save the next presented game frame (F12).
void Request();

/// Present thread: the number of screenshots requested since the last call.
unsigned Consume();

} // namespace Ps4RtScreenshot
