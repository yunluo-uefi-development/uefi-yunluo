// SPDX-License-Identifier: BSD-2-Clause-Patent
DefinitionBlock ("", "SSDT", 2, "YUNLUO", "WINTCH", 0x00000001)
{
    Scope (\_SB)
    {
        Device (YTCH)
        {
            Name (_HID, "YUNT0001")
            Name (_UID, Zero)
            Name (_DDN, "yunluo NT36523 touchscreen")
            Name (_STA, 0x0F)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x10005000, 0x00001000)
            })
        }
    }
}
