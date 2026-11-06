// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef YUNLUO_TOUCH_PLATFORM_H
#define YUNLUO_TOUCH_PLATFORM_H
#include <Uefi.h>

#define YUNLUO_TOUCH_PLATFORM_PROTOCOL_GUID \
  { 0x6f6d7fb2, 0x2c04, 0x4f2a, { 0x9a, 0x51, 0x4e, 0x8b, 0x13, 0x5a, 0x60, 0x19 } }

extern EFI_GUID gYunluoTouchPlatformProtocolGuid;

typedef struct _YUNLUO_TOUCH_PLATFORM_PROTOCOL YUNLUO_TOUCH_PLATFORM_PROTOCOL;
typedef struct {
  UINT64  GpioBase;
  UINT32  GpioRegionSize;
  UINT32  SpiMmioBase;
  UINT32  SpiMmioSize;
  UINT8   SpiPins[4];
  UINT8   TouchIrqGpio;
  UINT8   Mode;
  UINT8   Reserved[2];
} YUNLUO_TOUCH_PLATFORM_INFO;

typedef EFI_STATUS (EFIAPI *YUNLUO_TOUCH_GET_INFO)(
  IN  YUNLUO_TOUCH_PLATFORM_PROTOCOL  *This,
  OUT YUNLUO_TOUCH_PLATFORM_INFO      *Info
  );

struct _YUNLUO_TOUCH_PLATFORM_PROTOCOL {
  UINT32                    Revision;
  YUNLUO_TOUCH_GET_INFO     GetInfo;
};

#endif
