#ifndef GUARD_FRAMEDRAW_H
#define GUARD_FRAMEDRAW_H

#include "global.h"

void DrawFrame(uint16_t *pixels);
bool CopyFrameRegion(const uint16_t *sourcePixels, uint16_t *destPixels, int destStridePixels, int srcX, int srcY, int width, int height);
bool DrawFrameRegion(uint16_t *destPixels, int destStridePixels, int srcX, int srcY, int width, int height);
bool DrawFrameTopLayers(uint16_t *framePixels, uint16_t *topBgPixels, uint16_t *topSpritePixels);
#endif