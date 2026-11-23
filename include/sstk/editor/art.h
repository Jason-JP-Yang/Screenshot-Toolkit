// The editing panel's drawn parts: its icons, the markup tools as they stand in the toolbar, swatches, the crop
// handles, and the pieces of its popovers. Sizes are pixels, and `scale` is pixels per device independent pixel.
// Colors are 0xRRGGBB.
#pragma once

#include "sstk/editor/document.h"
#include "sstk/graphics/image.h"

namespace sstk {

enum class EditorIcon { kClose, kReset, kUndo, kRedo, kRotateRight, kRotateLeft, kDone, kFold, kMarkup };

Image RenderEditorIcon(EditorIcon icon, int size, float scale, uint32_t color);

// A filled rounded box, and the same with a hairline inside its edge.
Image RenderBox(int width, int height, float radius, uint32_t color, float alpha);
Image RenderPanel(int width, int height, float radius, uint32_t fill, uint32_t hairline, float hairline_alpha);

struct ToolLook {
  uint32_t barrel;
  uint32_t shade;
  uint32_t edge;
  uint32_t ink;     // The tool's color.
  int percent = -1;  // Its opacity, printed on the barrel below 100.
  uint32_t label;
  int label_turns = 0;  // Quarter turns clockwise the tool will be shown turned by, which its label is turned back from.
};

// A tool standing upright, tip at the top. The bottom runs on past what the toolbar shows.
Image RenderToolArt(Tool tool, int width, int height, float scale, const ToolLook& look);

// A color to choose, ringed when chosen. The last swatch is a ring of every hue around the color picked from the grid.
Image RenderSwatch(int size, float scale, uint32_t color, bool chosen, uint32_t panel);
Image RenderHueSwatch(int size, float scale, uint32_t center, bool filled, bool chosen, uint32_t panel);

// A crop frame's handle: an L for a corner (0 top left, 1 top right, 2 bottom left, 3 bottom right), or a bar along
// an edge. `pad` clear pixels on every side carry the halo.
Image RenderCornerHandle(int corner, int arm, int thickness, int pad, uint32_t color, uint32_t halo, float halo_alpha);
Image RenderEdgeHandle(bool across, int length, int thickness, int pad, uint32_t color, uint32_t halo, float halo_alpha);

// The opacity slider: checks fading into the color, and its knob.
Image RenderOpacityTrack(int width, int height, float scale, uint32_t color);
Image RenderOpacityKnob(int size, float scale, uint32_t color, float opacity);

// The color grid and the colors in it.
uint32_t GridColor(int column, int row);
Image RenderColorGrid(int columns, int rows, int cell, float scale, int chosen_column, int chosen_row);

// The ruler: a translucent band with marks every few pixels, `length` long.
Image RenderRuler(int length, int thickness, float scale);

}  // namespace sstk
