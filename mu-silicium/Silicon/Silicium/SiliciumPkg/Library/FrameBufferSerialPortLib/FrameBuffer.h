#ifndef _FRAME_BUFFER_H_
#define _FRAME_BUFFER_H_

//
// Frame Buffer Details
//
#define FB_WIDTH     FixedPcdGet32 (PcdFrameBufferWidth)
#define FB_HEIGHT    FixedPcdGet32 (PcdFrameBufferHeight)
// LK hands over a portrait 1200x2000 framebuffer with a 4800-byte pitch
// (1200 pixels per line, 4 bytes per pixel) on yunluo.
#define FB_STRIDE    1200
#define FB_BPP      (FixedPcdGet8  (PcdFrameBufferColorDepth) / 8)
#define FB_DELAY_US  FixedPcdGet32 (PcdFrameBufferDelay)

//
// Frame Buffer Position
//
typedef struct {
  UINT32 XPos;
  UINT32 YPos;
} EFI_FRAME_BUFFER_POSITION;

#endif /* _FRAME_BUFFER_H_ */
