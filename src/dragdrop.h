// Drags a saved screenshot file out of the overlay into other applications.
#pragma once

#include "image.h"
#include "platform.h"

#include <string>

namespace sstk {

// Blocks in the system drag loop until the button is released. Returns the resulting DROPEFFECT.
DWORD RunFileDrag(const std::wstring& path, const Image& preview);

}  // namespace sstk
