// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

typedef struct {
  UINT32  Address;
  UINT32  Flip;
} OVL_SCANOUT_LAYOUT;

STATIC
BOOLEAN
ComputeOvlScanout (
  IN  UINT64              Base,
  IN  UINT32              Width,
  IN  UINT32              Height,
  IN  UINT32              Pitch,
  IN  UINT32              Rotation,
  OUT OVL_SCANOUT_LAYOUT  *Layout
  )
{
  UINT64  Last;

  if ((Layout == NULL) || (Width == 0) || (Height == 0) ||
      (Width > 0xFFFF) || (Height > 0xFFFF) || ((Pitch & 3) != 0) ||
      ((UINT64)Width * 4 > Pitch) || (Base > 0xFFFFFFFFULL) ||
      ((Rotation != 0) && (Rotation != 180))) {
    return FALSE;
  }

  // The MTK OVL flip starts from the last visible byte, not padded pitch end.
  Last = (UINT64)(Height - 1) * Pitch + (UINT64)Width * 4 - 1;
  if (Last > 0xFFFFFFFFULL - Base) {
    return FALSE;
  }

  Layout->Address = (UINT32)(Base + ((Rotation == 180) ? Last : 0));
  Layout->Flip    = (Rotation == 180) ? (BIT9 | BIT10) : 0;
  return TRUE;
}
