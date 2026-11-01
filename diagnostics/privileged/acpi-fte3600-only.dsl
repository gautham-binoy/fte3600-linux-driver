
/* ===== dsdt.dsl: Device (FP01) lines 70234-70282 ===== */
        Device (FP01)
        {
            Name (_HID, "FTE3600")  
            Name (_CID, "FTE3600")  
            Method (_INI, 0, NotSerialized)  
            {
                SHPO (0x090E0003, One)
                SHPO (0x0900000E, One)
            }

            Method (_STA, 0, NotSerialized)  
            {
                If ((FPID == Zero))
                {
                    Return (0x0F)
                }

                Return (Zero)
            }

            Method (_CRS, 0, Serialized)  
            {
                Name (FBUF, ResourceTemplate ()
                {
                    SpiSerialBusV2 (0x0000, PolarityLow, FourWireMode, 0x08,
                        ControllerInitiated, 0x000F4240, ClockPolarityLow,
                        ClockPhaseFirst, "\\_SB.PC00.SPI1",
                        0x00, ResourceConsumer, , Exclusive,
                        )
                    GpioIo (Exclusive, PullUp, 0x0000, 0x0000, IoRestrictionOutputOnly,
                        "\\_SB.GPI0", 0x00, ResourceConsumer, ,
                        )
                        {   
                            0x0008
                        }
                    GpioInt (Edge, ActiveHigh, ExclusiveAndWake, PullNone, 0x0000,
                        "\\_SB.GPI0", 0x00, ResourceConsumer, ,
                        )
                        {   
                            0x0000
                        }
                })
                CreateWordField (FBUF, 0x3B, RSTP)
                CreateWordField (FBUF, 0x5E, INTP)
                RSTP = GNUM (0x0900000E)
                INTP = GNUM (0x090E0003)
                Return (FBUF) 
            }
        }
