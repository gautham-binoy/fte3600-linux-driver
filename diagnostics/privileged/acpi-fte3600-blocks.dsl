
/* ===== dsdt.dsl: Device (SPI1) lines 20322-20367 ===== */
        Device (SPI1)
        {
            If ((SM01 == 0x02))
            {
                Name (_HID, "PNP0C02" )  
                Method (_CRS, 0, NotSerialized)  
                {
                    Return (SPIH (SC01))
                }

                Name (_STA, 0x0B)  
            }

            If ((SM01 == One))
            {
                Method (_DSM, 4, Serialized)  
                {
                    If (PCIC (Arg0))
                    {
                        Return (PCID (Arg0, Arg1, Arg2, Arg3))
                    }

                    Return (Buffer (One)
                    {
                         0x00                                             
                    })
                }

                Method (_PS3, 0, NotSerialized)  
                {
                    SOD3 (SC01, One, One)
                }

                Method (_PS0, 0, NotSerialized)  
                {
                }
            }

            If (((SM01 == One) || (SM01 == Zero)))
            {
                Method (_ADR, 0, NotSerialized)  
                {
                    Return (0x001E0003)
                }
            }
        }

/* ===== dsdt.dsl: Device (PEPD) lines 48913-52078 ===== */
        Device (PEPD)
        {
            Name (_HID, "INT33A1" )  
            Name (_CID, EisaId ("PNP0D80") )  
            Name (_UID, One)  
            Name (LBUF, Buffer (0xC0){})
            Name (PPD0, Package (0x03)
            {
                "\\_SB.PC00.SAT0", 
                Zero, 
                Package (0x02)
                {
                    Zero, 
                    Package (0x03)
                    {
                        0xFF, 
                        Zero, 
                        0x81
                    }
                }
            })
            Name (PPD3, Package (0x03)
            {
                "\\_SB.PC00.SAT0", 
                Zero, 
                Package (0x02)
                {
                    Zero, 
                    Package (0x02)
                    {
                        0xFF, 
                        0x03
                    }
                }
            })
            Name (WWD3, Package (0x03)
            {
                "\\_SB.PC00.RP04", 
                Zero, 
                Package (0x02)
                {
                    Zero, 
                    Package (0x02)
                    {
                        0xFF, 
                        0x03
                    }
                }
            })
            Name (HDD3, Package (0x03)
            {
                "\\_SB.PC00.HDAS", 
                Zero, 
                Package (0x02)
                {
                    Zero, 
                    Package (0x02)
                    {
                        0xFF, 
                        0x03
                    }
                }
            })
            Name (HDD0, Package (0x03)
            {
                "\\_SB.PC00.HDAS", 
                Zero, 
                Package (0x02)
                {
                    Zero, 
                    Package (0x03)
                    {
                        0xFF, 
                        Zero, 
                        0x81
                    }
                }
            })
            Name (DEVY, Package (0x5B)
            {
                Package (0x03)
                {
                    "\\_SB.PR00", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            Zero
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PR01", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            Zero
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PR02", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            Zero
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PR03", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            Zero
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PR04", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            Zero
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PR05", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            Zero
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PR06", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            Zero
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PR07", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            Zero
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.GFX0", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.SAT0", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.UA00", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.UA01", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.I2C0", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.I2C1", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.XHCI", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.HDAS", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.PEMC", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.PSDC", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.I2C2", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.I2C3", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.I2C4", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.I2C5", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.UA02", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.SPI0", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.SPI1", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP01.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP02.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP03.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP04.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP05.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP06.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP07.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP08.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP09.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP10.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP11.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP12.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP13.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP14.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP15.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP16.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP17", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP18.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP19.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP20.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.SAT0.VOL0", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PR08", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            Zero
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PR09", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            Zero
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PR10", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            Zero
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PR11", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            Zero
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PR12", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            Zero
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PR13", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            Zero
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PR14", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            Zero
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PR15", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            Zero
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.SPI2", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.CNVW", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.IPU0", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.HECI", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.GLAN", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.PEG0.PEGP", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.THC0", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.THC1", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.HDAS.IDA", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "USB\\VID_8087&PID_0AC9&MI*", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x02
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.I2C6", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP04", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.TXHC", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.TXDC", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.TRP0", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.TRP1", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.TRP2", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.TRP3", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC01.TRP0", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC01.TRP1", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC01.TRP2", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC01.TRP3", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.TDM0", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.TDM1", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.GNA0", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.VMD0", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP21.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP22.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP23.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP24.PXSX", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.PEG0", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.RP09", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.PEG1.PEGP", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.PEG2.PEGP", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.PEG3.PEGP", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.HEC3", 
                    One, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x03)
                        {
                            0xFF, 
                            Zero, 
                            0x81
                        }
                    }
                }, 

                Package (0x03)
                {
                    "\\_SB.PC00.PEG1", 
                    Zero, 
                    Package (0x02)
                    {
                        Zero, 
                        Package (0x02)
                        {
                            0xFF, 
                            0x03
                        }
                    }
                }
            })
            Name (BCCD, Package (0x1D)
            {
                Package (0x02)
                {
                    "\\_SB.PC00.SAT0", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x3E80
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.SAT0.PRT0", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x3E80
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.SAT0.PRT1", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x3E80
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.SAT0.PRT2", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x3E80
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.SAT0.PRT3", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x3E80
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP01.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP02.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP03.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP04.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP05.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP06.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP07.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP08.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP09.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP10.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP11.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP12.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP13.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP14.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP15.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP16.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP17.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP18.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP19.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP20.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP21.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP22.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP23.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }, 

                Package (0x02)
                {
                    "\\_SB.PC00.RP24.PXSX", 
                    Package (0x01)
                    {
                        Package (0x03)
                        {
                            Package (0x05)
                            {
                                One, 
                                0x08, 
                                Zero, 
                                One, 
                                0xB2
                            }, 

                            Package (0x03)
                            {
                                Zero, 
                                0xCD, 
                                One
                            }, 

                            0x000186A0
                        }
                    }
                }
            })
            OperationRegion (LS0I, SystemMemory, LPMR, 0xC0)
            Field (LS0I, ByteAcc, Lock, Preserve)
            {
                L0IX,   1536
            }

            Method (_STA, 0, NotSerialized)  
            {
                If (((OSYS >= 0x07DF) || ((OSYS >= 0x07DC) && (
                    S0ID == One))))
                {
                    Return (0x0F)
                }

                Return (Zero)
            }

            Method (_DSM, 4, Serialized)  
            {
                If ((Arg0 == ToUUID ("c4eb40a0-6cd2-11e2-bcfd-0800200c9a66") ))
                {
                    If ((Arg2 == Zero))
                    {
                        Return (Buffer (One)
                        {
                             0x7F                                             
                        })
                    }

                    If ((Arg2 == One))
                    {
                        If ((S0ID == Zero))
                        {
                            Return (Package (0x00){})
                        }

                        If (((PEPC & One) == One))
                        {
                            If (((PEPC & 0x03) == One))
                            {
                                DEVY [0x09] = PPD0 
                            }
                            ElseIf (((PEPC & 0x03) == 0x03))
                            {
                                DEVY [0x09] = PPD3 
                            }

                            DerefOf (DEVY [0x09]) [One] = One
                        }
                        ElseIf (((PEPC & 0x03) == 0x02))
                        {
                            DerefOf (DEVY [0x2D]) [One] = One
                        }

                        If (((PEPC & 0x10) == 0x10))
                        {
                            If ((^^PC00.RP01.PXSX.PAHC () || ^^PC00.RP01.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x19]) [One] = One
                            }

                            If ((^^PC00.RP02.PXSX.PAHC () || ^^PC00.RP02.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x1A]) [One] = One
                            }

                            If ((^^PC00.RP03.PXSX.PAHC () || ^^PC00.RP03.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x1B]) [One] = One
                            }

                            If ((^^PC00.RP04.PXSX.PAHC () || ^^PC00.RP04.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x1C]) [One] = One
                            }

                            If ((^^PC00.RP05.PXSX.PAHC () || ^^PC00.RP05.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x1D]) [One] = One
                            }

                            If ((^^PC00.RP06.PXSX.PAHC () || ^^PC00.RP06.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x1E]) [One] = One
                            }

                            If ((^^PC00.RP07.PXSX.PAHC () || ^^PC00.RP07.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x1F]) [One] = One
                            }

                            If ((^^PC00.RP08.PXSX.PAHC () || ^^PC00.RP08.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x20]) [One] = One
                            }

                            If ((^^PC00.RP09.PXSX.PAHC () || ^^PC00.RP09.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x21]) [One] = One
                            }

                            If ((^^PC00.RP10.PXSX.PAHC () || ^^PC00.RP10.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x22]) [One] = One
                            }

                            If ((^^PC00.RP11.PXSX.PAHC () || ^^PC00.RP11.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x23]) [One] = One
                            }

                            If ((^^PC00.RP12.PXSX.PAHC () || ^^PC00.RP12.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x24]) [One] = One
                            }

                            If ((^^PC00.RP13.PXSX.PAHC () || ^^PC00.RP13.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x25]) [One] = One
                            }

                            If ((^^PC00.RP14.PXSX.PAHC () || ^^PC00.RP14.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x26]) [One] = One
                            }

                            If ((^^PC00.RP15.PXSX.PAHC () || ^^PC00.RP15.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x27]) [One] = One
                            }

                            If ((^^PC00.RP16.PXSX.PAHC () || ^^PC00.RP16.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x28]) [One] = One
                            }

                            If ((^^PC00.RP17.PXSX.PAHC () || ^^PC00.RP17.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x29]) [One] = One
                            }

                            If ((^^PC00.RP18.PXSX.PAHC () || ^^PC00.RP18.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x2A]) [One] = One
                            }

                            If ((^^PC00.RP19.PXSX.PAHC () || ^^PC00.RP19.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x2B]) [One] = One
                            }

                            If ((^^PC00.RP20.PXSX.PAHC () || ^^PC00.RP20.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x2C]) [One] = One
                            }

                            If ((^^PC00.RP21.PXSX.PAHC () || ^^PC00.RP21.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x50]) [One] = One
                            }

                            If ((^^PC00.RP22.PXSX.PAHC () || ^^PC00.RP22.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x51]) [One] = One
                            }

                            If ((^^PC00.RP23.PXSX.PAHC () || ^^PC00.RP23.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x52]) [One] = One
                            }

                            If ((^^PC00.RP24.PXSX.PAHC () || ^^PC00.RP24.PXSX.PNVM ()))
                            {
                                DerefOf (DEVY [0x53]) [One] = One
                            }
                        }

                        If (((OSYS >= 0x07DF) && (WRTO != Zero)))
                        {
                            If (^^PC00.RP04.PXSX.WWST ())
                            {
                                If ((WRTO == 0x03))
                                {
                                    WWD3 [Zero] = "\\_SB.PC00.RP04"
                                    DEVY [0x41] = WWD3 
                                    DerefOf (DEVY [0x41]) [One] = One
                                }

                                If ((WRTO == One))
                                {
                                    DerefOf (DEVY [0x3F]) [One] = One
                                }
                            }
                            ElseIf (^^PC00.RP09.PXSX.WWST ())
                            {
                                If ((WRTO == 0x03))
                                {
                                    WWD3 [Zero] = "\\_SB.PC00.RP09"
                                    DEVY [0x55] = WWD3 
                                    DerefOf (DEVY [0x55]) [One] = One
                                }

                                If ((WRTO == One))
                                {
                                    DerefOf (DEVY [0x3F]) [One] = One
                                }
                            }
                        }

                        If (((PEPC & 0x04) == Zero))
                        {
                            DerefOf (DEVY [0x0A]) [One] = Zero
                        }

                        If (((PEPC & 0x08) == Zero))
                        {
                            DerefOf (DEVY [0x0B]) [One] = Zero
                        }

                        If (((PEPC & 0x10) == Zero)){}
                        If (((PEPC & 0x20) == Zero))
                        {
                            DerefOf (DEVY [0x0C]) [One] = Zero
                        }

                        If (((PEPC & 0x40) == Zero))
                        {
                            DerefOf (DEVY [0x0D]) [One] = Zero
                        }

                        If (((PEPC & 0x80) == Zero))
                        {
                            DerefOf (DEVY [0x0E]) [One] = Zero
                        }

                        If (((PEPC & 0x0100) == 0x0100))
                        {
                            If (((PEPC & 0x0300) == 0x0100))
                            {
                                DEVY [0x0F] = HDD0 
                            }
                            ElseIf (((PEPC & 0x0300) == 0x0300))
                            {
                                DEVY [0x0F] = HDD3 
                            }

                            DerefOf (DEVY [0x0F]) [One] = One
                        }

                        If (((PEPC & 0x0400) == Zero))
                        {
                            DerefOf (DEVY [0x08]) [One] = Zero
                        }

                        If ((TCNT >= One))
                        {
                            DerefOf (DEVY [Zero]) [One] = One
                        }

                        If ((TCNT >= 0x02))
                        {
                            DerefOf (DEVY [One]) [One] = One
                        }

                        If ((TCNT >= 0x03))
                        {
                            DerefOf (DEVY [0x02]) [One] = One
                        }

                        If ((TCNT >= 0x04))
                        {
                            DerefOf (DEVY [0x03]) [One] = One
                        }

                        If ((TCNT >= 0x05))
                        {
                            DerefOf (DEVY [0x04]) [One] = One
                        }

                        If ((TCNT >= 0x06))
                        {
                            DerefOf (DEVY [0x05]) [One] = One
                        }

                        If ((TCNT >= 0x07))
                        {
                            DerefOf (DEVY [0x06]) [One] = One
                        }

                        If ((TCNT >= 0x08))
                        {
                            DerefOf (DEVY [0x07]) [One] = One
                        }

                        If ((TCNT >= 0x09))
                        {
                            DerefOf (DEVY [0x2E]) [One] = One
                        }

                        If ((TCNT >= 0x0A))
                        {
                            DerefOf (DEVY [0x2F]) [One] = One
                        }

                        If ((TCNT >= 0x0B))
                        {
                            DerefOf (DEVY [0x30]) [One] = One
                        }

                        If ((TCNT >= 0x0C))
                        {
                            DerefOf (DEVY [0x31]) [One] = One
                        }

                        If ((TCNT >= 0x0D))
                        {
                            DerefOf (DEVY [0x32]) [One] = One
                        }

                        If ((TCNT >= 0x0E))
                        {
                            DerefOf (DEVY [0x33]) [One] = One
                        }

                        If ((TCNT >= 0x0F))
                        {
                            DerefOf (DEVY [0x34]) [One] = One
                        }

                        If ((TCNT >= 0x10))
                        {
                            DerefOf (DEVY [0x35]) [One] = One
                        }

                        If (((PEPC & 0x0800) == Zero))
                        {
                            DerefOf (DEVY [Zero]) [One] = Zero
                            DerefOf (DEVY [One]) [One] = Zero
                            DerefOf (DEVY [0x02]) [One] = Zero
                            DerefOf (DEVY [0x03]) [One] = Zero
                            DerefOf (DEVY [0x04]) [One] = Zero
                            DerefOf (DEVY [0x05]) [One] = Zero
                            DerefOf (DEVY [0x06]) [One] = Zero
                            DerefOf (DEVY [0x07]) [One] = Zero
                            DerefOf (DEVY [0x2E]) [One] = Zero
                            DerefOf (DEVY [0x2F]) [One] = Zero
                            DerefOf (DEVY [0x30]) [One] = Zero
                            DerefOf (DEVY [0x31]) [One] = Zero
                            DerefOf (DEVY [0x32]) [One] = Zero
                            DerefOf (DEVY [0x33]) [One] = Zero
                            DerefOf (DEVY [0x34]) [One] = Zero
                            DerefOf (DEVY [0x35]) [One] = Zero
                        }

                        If ((((PEPC & 0x1000) == Zero) || (EMCE == Zero)))
                        {
                            DerefOf (DEVY [0x10]) [One] = Zero
                        }

                        If ((((PEPC & 0x2000) == Zero) || (SDCE == Zero)))
                        {
                            DerefOf (DEVY [0x11]) [One] = Zero
                        }

                        If (((PEPC & 0x4000) == Zero))
                        {
                            DerefOf (DEVY [0x12]) [One] = Zero
                        }

                        If (((PEPC & 0x8000) == Zero))
                        {
                            DerefOf (DEVY [0x13]) [One] = Zero
                        }

                        If (((PEPC & 0x00010000) == Zero))
                        {
                            DerefOf (DEVY [0x14]) [One] = Zero
                        }

                        If (((PEPC & 0x00020000) == Zero))
                        {
                            DerefOf (DEVY [0x15]) [One] = Zero
                        }

                        If (((PEPC & 0x00040000) == Zero))
                        {
                            DerefOf (DEVY [0x16]) [One] = Zero
                        }

                        If (((PEPC & 0x00080000) == Zero))
                        {
                            DerefOf (DEVY [0x17]) [One] = Zero
                        }

                        If (((PEPC & 0x00100000) == Zero))
                        {
                            DerefOf (DEVY [0x18]) [One] = Zero
                        }

                        If (((PEPC & 0x00200000) == Zero))
                        {
                            DerefOf (DEVY [0x36]) [One] = Zero
                        }

                        If (((PEPC & 0x00400000) == Zero))
                        {
                            DerefOf (DEVY [0x38]) [One] = Zero
                        }

                        If (((PEPC & 0x00800000) == Zero))
                        {
                            DerefOf (DEVY [0x39]) [One] = Zero
                        }

                        If ((((PEPC & 0x01000000) == Zero) || (GBES == Zero)))
                        {
                            DerefOf (DEVY [0x3A]) [One] = Zero
                        }

                        If (((PEPC & 0x02000000) != Zero))
                        {
                            If ((^^PC00.PEG0.PEGP.PAHC () || ^^PC00.PEG0.PEGP.PNVM ()))
                            {
                                DerefOf (DEVY [0x3B]) [One] = One
                            }

                            If (CondRefOf (\_SB.PC00.PEG1))
                            {
                                If ((^^PC00.PEG1.PEGP.PAHC () || ^^PC00.PEG1.PEGP.PNVM ()))
                                {
                                    DerefOf (DEVY [0x56]) [One] = One
                                }
                            }

                            If (CondRefOf (\_SB.PC00.PEG2))
                            {
                                If ((^^PC00.PEG2.PEGP.PAHC () || ^^PC00.PEG2.PEGP.PNVM ()))
                                {
                                    DerefOf (DEVY [0x57]) [One] = One
                                }
                            }

                            If (CondRefOf (\_SB.PC00.PEG3))
                            {
                                If ((^^PC00.PEG3.PEGP.PAHC () || ^^PC00.PEG3.PEGP.PNVM ()))
                                {
                                    DerefOf (DEVY [0x58]) [One] = One
                                }
                            }
                        }

                        If (((PEPC & 0x04000000) == Zero))
                        {
                            DerefOf (DEVY [0x3C]) [One] = Zero
                        }

                        If (((PEPC & 0x08000000) == Zero))
                        {
                            DerefOf (DEVY [0x3D]) [One] = Zero
                        }

                        DerefOf (DEVY [0x3E]) [One] = Zero
                        If (((PEPC & 0x20000000) == Zero))
                        {
                            DerefOf (DEVY [0x40]) [One] = Zero
                        }

                        If (((PEPC & 0x40000000) != Zero))
                        {
                            If ((THCE == One))
                            {
                                DerefOf (DEVY [0x42]) [One] = One
                            }

                            If ((TDCE == One))
                            {
                                DerefOf (DEVY [0x43]) [One] = One
                            }

                            If (CondRefOf (\_SB.PC01))
                            {
                                If ((TRE0 == One))
                                {
                                    DerefOf (DEVY [0x48]) [One] = One
                                }

                                If ((TRE1 == One))
                                {
                                    DerefOf (DEVY [0x49]) [One] = One
                                }

                                If ((TRE2 == One))
                                {
                                    DerefOf (DEVY [0x4A]) [One] = One
                                }

                                If ((TRE3 == One))
                                {
                                    DerefOf (DEVY [0x4B]) [One] = One
                                }
                            }
                            Else
                            {
                                If ((TRE0 == One))
                                {
                                    DerefOf (DEVY [0x44]) [One] = One
                                }

                                If ((TRE1 == One))
                                {
                                    DerefOf (DEVY [0x45]) [One] = One
                                }

                                If ((TRE2 == One))
                                {
                                    DerefOf (DEVY [0x46]) [One] = One
                                }

                                If ((TRE3 == One))
                                {
                                    DerefOf (DEVY [0x47]) [One] = One
                                }
                            }

                            If ((DME0 == One))
                            {
                                DerefOf (DEVY [0x4C]) [One] = One
                            }

                            If ((DME1 == One))
                            {
                                DerefOf (DEVY [0x4D]) [One] = One
                            }
                        }

                        If (((PEPC & 0x80000000) == Zero))
                        {
                            DerefOf (DEVY [0x4E]) [One] = Zero
                        }

                        If (((PEPC & 0x0000000100000000) == Zero))
                        {
                            DerefOf (DEVY [0x4F]) [One] = Zero
                        }

                        If (((PEPC & 0x0000000200000000) != Zero))
                        {
                            DerefOf (DEVY [0x54]) [One] = One
                        }

                        If (((PEPC & 0x0000000400000000) == Zero))
                        {
                            DerefOf (DEVY [0x59]) [One] = Zero
                        }

                        If (((PEPC & 0x0000000800000000) != Zero))
                        {
                            DerefOf (DEVY [0x5A]) [One] = One
                        }

                        If ((OSYS >= 0x07DF))
                        {
                            If (^^PC00.RP01.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x19]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x19]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x19]) [One] = One
                            }

                            If (^^PC00.RP02.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x1A]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x1A]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x1A]) [One] = One
                            }

                            If (^^PC00.RP03.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x1B]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x1B]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x1B]) [One] = One
                            }

                            If (^^PC00.RP04.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x1C]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x1C]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x1C]) [One] = One
                            }

                            If (^^PC00.RP05.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x1D]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x1D]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x1D]) [One] = One
                            }

                            If (^^PC00.RP06.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x1E]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x1E]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x1E]) [One] = One
                            }

                            If (^^PC00.RP07.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x1F]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x1F]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x1F]) [One] = One
                            }

                            If (^^PC00.RP08.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x20]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x20]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x20]) [One] = One
                            }

                            If (^^PC00.RP09.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x21]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x21]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x21]) [One] = One
                            }

                            If (^^PC00.RP10.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x22]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x22]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x22]) [One] = One
                            }

                            If (^^PC00.RP11.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x23]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x23]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x23]) [One] = One
                            }

                            If (^^PC00.RP12.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x24]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x24]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x24]) [One] = One
                            }

                            If (^^PC00.RP13.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x25]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x25]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x25]) [One] = One
                            }

                            If (^^PC00.RP14.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x26]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x26]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x26]) [One] = One
                            }

                            If (^^PC00.RP15.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x27]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x27]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x27]) [One] = One
                            }

                            If (^^PC00.RP16.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x28]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x28]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x28]) [One] = One
                            }

                            If (^^PC00.RP17.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x29]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x29]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x29]) [One] = One
                            }

                            If (^^PC00.RP18.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x2A]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x2A]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x2A]) [One] = One
                            }

                            If (^^PC00.RP19.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x2B]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x2B]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x2B]) [One] = One
                            }

                            If (^^PC00.RP20.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x2C]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x2C]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x2C]) [One] = One
                            }

                            If (^^PC00.RP21.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x50]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x50]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x50]) [One] = One
                            }

                            If (^^PC00.RP22.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x51]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x51]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x51]) [One] = One
                            }

                            If (^^PC00.RP23.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x52]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x52]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x52]) [One] = One
                            }

                            If (^^PC00.RP24.PXSX.WIST ())
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x53]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x53]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x53]) [One] = One
                            }

                            If ((^^PC00.CNVW.VDID != 0xFFFFFFFF))
                            {
                                DerefOf (DerefOf (DerefOf (DEVY [0x37]) [0x02]
                                    ) [One]) [One] = 0x03
                                DerefOf (DerefOf (DerefOf (DEVY [0x37]) [0x02]
                                    ) [One]) [0x02] = Zero
                                DerefOf (DEVY [0x37]) [One] = One
                            }
                        }

                        Return (DEVY) 
                    }

                    If ((Arg2 == 0x02))
                    {
                        Return (BCCD) 
                    }

                    If ((Arg2 == 0x03))
                    {
                        If ((S0ID == One))
                        {
                            D8XH (Zero, 0xC5)
                            D8XH (One, Zero)
                        }
                    }

                    If ((Arg2 == 0x04))
                    {
                        If ((S0ID == One))
                        {
                            D8XH (Zero, 0xC5)
                            D8XH (One, 0xAB)
                        }
                    }

                    If ((Arg2 == 0x05))
                    {
                        If ((S0ID == One))
                        {
                            GUAM (One)
                        }

                        If (ECON)
                        {
                            ^^PC00.LPCB.H_EC.ECNT (One)
                        }

                        If (CondRefOf (\_SB.PC00.TXHC))
                        {
                            ^^PC00.DPOF = One
                        }

                        ^^PC00.LPCB.H_EC.MSEC = One
                    }

                    If ((Arg2 == 0x06))
                    {
                        If ((S0ID == One))
                        {
                            GUAM (Zero)
                        }

                        If (ECON)
                        {
                            ^^PC00.LPCB.H_EC.ECNT (Zero)
                        }

                        If (CondRefOf (\_SB.PC00.TXHC))
                        {
                            ^^PC00.DPOF = Zero
                        }

                        ^^PC00.LPCB.H_EC.MSEC = Zero
                    }
                }

                If ((Arg0 == ToUUID ("57a6512e-3979-4e9d-9708-ff13b2508972") ))
                {
                    If ((Arg2 == Zero))
                    {
                        Return (Buffer (One)
                        {
                             0x03                                             
                        })
                    }

                    If ((Arg2 == One))
                    {
                        LBUF = L0IX 
                        Return (LBUF) 
                    }
                }

                Return (Buffer (One)
                {
                     0x00                                             
                })
            }
        }

/* ===== dsdt.dsl: Device (LNK1) lines 56512-57027 ===== */
        Device (LNK1)
        {
            Name (_ADR, Zero)  
            Name (PNIO, Package (0x01)
            {
                "\\_SB.PC00.SPI1.SPFD.CVFD"
            })
            Name (PUSB, Package (0x02)
            {
                "\\_SB.PC00.SPI1.SPFD.CVFD", 
                "\\_SB.PC00.XHCI.RHUB.HS04.VI2C"
            })
            Method (_STA, 0, NotSerialized)  
            {
                If (L1EN)
                {
                    Return (0x0F)
                }
                Else
                {
                    Return (Zero)
                }
            }

            Method (_DEP, 0, Serialized)  
            {
                If (L1EN)
                {
                    Switch (CVFS)
                    {
                        Case (0x02)
                        {
                            Return (PUSB) 
                        }
                        Case (One)
                        {
                            Return (PNIO) 
                        }
                        Default
                        {
                            Return (CDEP (L1CL, L1BS))
                        }

                    }
                }
                Else
                {
                    Return (Package (0x01)
                    {
                        PC00
                    })
                }
            }

            Name (_UID, One)  
            Method (_HID, 0, NotSerialized)  
            {
                Return (HCID (One))
            }

            Method (_CID, 0, NotSerialized)  
            {
                Return (HCID (One))
            }

            Method (_DDN, 0, NotSerialized)  
            {
                Name (BUF, Buffer (0x10){})
                BUF [Zero] = L1M0 
                BUF [One] = L1M1 
                BUF [0x02] = L1M2 
                BUF [0x03] = L1M3 
                BUF [0x04] = L1M4 
                BUF [0x05] = L1M5 
                BUF [0x06] = L1M6 
                BUF [0x07] = L1M7 
                BUF [0x08] = L1M8 
                BUF [0x09] = L1M9 
                BUF [0x0A] = L1MA 
                BUF [0x0B] = L1MB 
                BUF [0x0C] = L1MC 
                BUF [0x0D] = L1MD 
                BUF [0x0E] = L1ME 
                BUF [0x0F] = L1MF 
                Return (ToString (BUF, Ones))
            }

            Method (_PLD, 0, Serialized)  
            {
                Name (PLDB, Package (0x01)
                {
                    Buffer (0x14)
                    {
                          0x82, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  
                          0x69, 0x0E, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,  
                          0xFF, 0xFF, 0xFF, 0xFF                           
                    }
                })
                CreateByteField (DerefOf (PLDB [Zero]), 0x08, BPOS)
                CreateField (DerefOf (PLDB [Zero]), 0x73, 0x04, RPOS)
                BPOS = L1PL 
                RPOS = L1DG 
                Return (PLDB) 
            }

            Method (_CRS, 0, NotSerialized)  
            {
                If ((CVFS == 0x02))
                {
                    Return (VIIC (L1A0))
                }

                Local0 = Buffer (0x02)
                    {
                         0x79, 0x00                                       
                    }
                If ((L1DI > Zero))
                {
                    Local1 = IICB (L1A0, L1BS)
                    ConcatenateResTemplate (Local0, Local1, Local2)
                    Local0 = Local2
                }

                If ((L1DI > One))
                {
                    Local1 = IICB (L1A1, L1BS)
                    ConcatenateResTemplate (Local0, Local1, Local2)
                    Local0 = Local2
                }

                If ((L1DI > 0x02))
                {
                    Local1 = IICB (L1A2, L1BS)
                    ConcatenateResTemplate (Local0, Local1, Local2)
                    Local0 = Local2
                }

                If ((L1DI > 0x03))
                {
                    Local1 = IICB (L1A3, L1BS)
                    ConcatenateResTemplate (Local0, Local1, Local2)
                    Local0 = Local2
                }

                If ((L1DI > 0x04))
                {
                    Local1 = IICB (L1A4, L1BS)
                    ConcatenateResTemplate (Local0, Local1, Local2)
                    Local0 = Local2
                }

                If ((L1DI > 0x05))
                {
                    Local1 = IICB (L1A5, L1BS)
                    ConcatenateResTemplate (Local0, Local1, Local2)
                    Local0 = Local2
                }

                If ((L1DI > 0x06))
                {
                    Local1 = IICB (L1A6, L1BS)
                    ConcatenateResTemplate (Local0, Local1, Local2)
                    Local0 = Local2
                }

                If ((L1DI > 0x07))
                {
                    Local1 = IICB (L1A7, L1BS)
                    ConcatenateResTemplate (Local0, Local1, Local2)
                    Local0 = Local2
                }

                If ((L1DI > 0x08))
                {
                    Local1 = IICB (L1A8, L1BS)
                    ConcatenateResTemplate (Local0, Local1, Local2)
                    Local0 = Local2
                }

                If ((L1DI > 0x09))
                {
                    Local1 = IICB (L1A9, L1BS)
                    ConcatenateResTemplate (Local0, Local1, Local2)
                    Local0 = Local2
                }

                If ((L1DI > 0x0A))
                {
                    Local1 = IICB (L1AA, L1BS)
                    ConcatenateResTemplate (Local0, Local1, Local2)
                    Local0 = Local2
                }

                If ((L1DI > 0x0B))
                {
                    Local1 = IICB (L1AB, L1BS)
                    ConcatenateResTemplate (Local0, Local1, Local2)
                    Local0 = Local2
                }

                Return (Local0)
            }

            Method (SSDB, 0, NotSerialized)
            {
                Name (PAR, Buffer (0x6C)
                {
                      0x00, 0x00, 0x69, 0x56, 0x39, 0x8A, 0xF7, 0x11,  
                      0xA9, 0x4E, 0x9C, 0x7D, 0x20, 0xEE, 0x0A, 0xB5,  
                      0xCA, 0x40, 0xA3, 0x00, 0x00, 0x00, 0x00, 0x00,  
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  
                      0x0C, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,  
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  
                      0x00, 0x00, 0x00, 0x00                           
                })
                PAR [Zero] = L1DV 
                PAR [One] = L1CV 
                PAR [0x18] = L1LC 
                PAR [0x1C] = L1LU 
                PAR [0x1D] = L1NL 
                PAR [0x4E] = L1EE 
                PAR [0x4F] = L1VC 
                If ((PCHS == PCHH))
                {
                    PAR [0x50] = 0x0D
                }

                PAR [0x52] = L1FS 
                PAR [0x53] = L1LE 
                PAR [0x54] = CDEG (L1DG)
                CreateDWordField (PAR, 0x56, DAT)
                DAT = L1CK 
                PAR [0x5A] = L1CL 
                PAR [0x5F] = L1PP 
                PAR [0x60] = L1VR 
                PAR [0x61] = L1PV 
                PAR [0x62] = L1PU 
                Return (PAR) 
            }

            Method (_DSM, 4, NotSerialized)  
            {
                If ((Arg0 == ToUUID ("822ace8f-2814-4174-a56b-5f029fe079ee") ))
                {
                    If ((Arg2 == Zero))
                    {
                        If ((Arg1 == Zero))
                        {
                            Return (Buffer (One)
                            {
                                 0x03                                             
                            })
                        }
                        Else
                        {
                            Return (Buffer (One)
                            {
                                 0x01                                             
                            })
                        }
                    }

                    If ((Arg2 == One))
                    {
                        Name (BUF, Buffer (0x10){})
                        BUF [Zero] = L1M0 
                        BUF [One] = L1M1 
                        BUF [0x02] = L1M2 
                        BUF [0x03] = L1M3 
                        BUF [0x04] = L1M4 
                        BUF [0x05] = L1M5 
                        BUF [0x06] = L1M6 
                        BUF [0x07] = L1M7 
                        BUF [0x08] = L1M8 
                        BUF [0x09] = L1M9 
                        BUF [0x0A] = L1MA 
                        BUF [0x0B] = L1MB 
                        BUF [0x0C] = L1MC 
                        BUF [0x0D] = L1MD 
                        BUF [0x0E] = L1ME 
                        BUF [0x0F] = L1MF 
                        Return (ToString (BUF, Ones))
                    }
                }

                If ((Arg0 == ToUUID ("5815c5c8-c47d-477b-9a8d-76173176414b") ))
                {
                    If ((Arg2 == Zero))
                    {
                        If ((Arg1 == Zero))
                        {
                            Return (Buffer (One)
                            {
                                 0x03                                             
                            })
                        }
                        Else
                        {
                            Return (Buffer (One)
                            {
                                 0x01                                             
                            })
                        }
                    }

                    If ((Arg2 == One))
                    {
                        Name (DSMB, Buffer (0x34){})
                        CreateDWordField (DSMB, Zero, I2CC)
                        CreateDWordField (DSMB, 0x04, DEV0)
                        CreateDWordField (DSMB, 0x08, DEV1)
                        CreateDWordField (DSMB, 0x0C, DEV2)
                        CreateDWordField (DSMB, 0x10, DEV3)
                        CreateDWordField (DSMB, 0x14, DEV4)
                        CreateDWordField (DSMB, 0x18, DEV5)
                        CreateDWordField (DSMB, 0x1C, DEV6)
                        CreateDWordField (DSMB, 0x20, DEV7)
                        CreateDWordField (DSMB, 0x24, DEV8)
                        CreateDWordField (DSMB, 0x28, DEV9)
                        CreateDWordField (DSMB, 0x2C, DEVA)
                        CreateDWordField (DSMB, 0x30, DEVB)
                        I2CC = L1DI 
                        DEV0 = DSMI (L1D0, L1A0, Zero, L1BS)
                        DEV1 = DSMI (L1D1, L1A1, Zero, L1BS)
                        DEV2 = DSMI (L1D2, L1A2, Zero, L1BS)
                        DEV3 = DSMI (L1D3, L1A3, Zero, L1BS)
                        DEV4 = DSMI (L1D4, L1A4, Zero, L1BS)
                        DEV5 = DSMI (L1D5, L1A5, Zero, L1BS)
                        DEV6 = DSMI (L1D6, L1A6, Zero, L1BS)
                        DEV7 = DSMI (L1D7, L1A7, Zero, L1BS)
                        DEV8 = DSMI (L1D8, L1A8, Zero, L1BS)
                        DEV9 = DSMI (L1D9, L1A9, Zero, L1BS)
                        DEVA = DSMI (L1DA, L1AA, Zero, L1BS)
                        DEVB = DSMI (L1DB, L1AB, Zero, L1BS)
                        Return (DSMB) 
                    }

                    Return (Buffer (One)
                    {
                         0x00                                             
                    })
                }

                If ((Arg0 == ToUUID ("26257549-9271-4ca4-bb43-c4899d5a4881") ))
                {
                    If ((Arg2 == Zero))
                    {
                        Return (Buffer (0x02)
                        {
                             0xFF, 0x1F                                       
                        })
                    }

                    If ((Arg2 == One))
                    {
                        Return (L1DI) 
                    }

                    If ((Arg2 == 0x02))
                    {
                        If ((L1DI > Zero))
                        {
                            Return (DSMI (L1D0, L1A0, Zero, L1BS))
                        }
                        Else
                        {
                            Return (Zero)
                        }
                    }

                    If ((Arg2 == 0x03))
                    {
                        If ((L1DI > One))
                        {
                            Return (DSMI (L1D1, L1A1, Zero, L1BS))
                        }
                        Else
                        {
                            Return (Zero)
                        }
                    }

                    If ((Arg2 == 0x04))
                    {
                        If ((L1DI > 0x02))
                        {
                            Return (DSMI (L1D2, L1A2, Zero, L1BS))
                        }
                        Else
                        {
                            Return (Zero)
                        }
                    }

                    If ((Arg2 == 0x05))
                    {
                        If ((L1DI > 0x03))
                        {
                            Return (DSMI (L1D3, L1A3, Zero, L1BS))
                        }
                        Else
                        {
                            Return (Zero)
                        }
                    }

                    If ((Arg2 == 0x06))
                    {
                        If ((L1DI > 0x04))
                        {
                            Return (DSMI (L1D4, L1A4, Zero, L1BS))
                        }
                        Else
                        {
                            Return (Zero)
                        }
                    }

                    If ((Arg2 == 0x07))
                    {
                        If ((L1DI > 0x05))
                        {
                            Return (DSMI (L1D5, L1A5, Zero, L1BS))
                        }
                        Else
                        {
                            Return (Zero)
                        }
                    }

                    If ((Arg2 == 0x08))
                    {
                        If ((L1DI > 0x06))
                        {
                            Return (DSMI (L1D6, L1A6, Zero, L1BS))
                        }
                        Else
                        {
                            Return (Zero)
                        }
                    }

                    If ((Arg2 == 0x08))
                    {
                        If ((L1DI > 0x07))
                        {
                            Return (DSMI (L1D7, L1A7, Zero, L1BS))
                        }
                        Else
                        {
                            Return (Zero)
                        }
                    }

                    If ((Arg2 == 0x09))
                    {
                        If ((L1DI > 0x08))
                        {
                            Return (DSMI (L1D8, L1A8, Zero, L1BS))
                        }
                        Else
                        {
                            Return (Zero)
                        }
                    }

                    If ((Arg2 == 0x0A))
                    {
                        If ((L1DI > 0x09))
                        {
                            Return (DSMI (L1D9, L1A9, Zero, L1BS))
                        }
                        Else
                        {
                            Return (Zero)
                        }
                    }

                    If ((Arg2 == 0x0B))
                    {
                        If ((L1DI > 0x0A))
                        {
                            Return (DSMI (L1DA, L1AA, Zero, L1BS))
                        }
                        Else
                        {
                            Return (Zero)
                        }
                    }

                    If ((Arg2 == 0x0C))
                    {
                        If ((L1DI > 0x0B))
                        {
                            Return (DSMI (L1DB, L1AB, Zero, L1BS))
                        }
                        Else
                        {
                            Return (Zero)
                        }
                    }
                }

                Return (Buffer (One)
                {
                     0x00                                             
                })
            }
        }

/* ===== dsdt.dsl: Device (FPNT) lines 61960-62140 ===== */
        Device (FPNT)
        {
            Method (_HID, 0, NotSerialized)  
            {
                If ((FPTT == One))
                {
                    Return ("FPC1011")
                }

                If ((FPTT == 0x02))
                {
                    Return ("FPC1020")
                }

                If ((FPTT == 0x03))
                {
                    Return ("VFSI6101")
                }

                If ((FPTT == 0x04))
                {
                    Return ("VFSI7500")
                }

                If ((FPTT == 0x05))
                {
                    Return ("EGIS0300")
                }

                If ((FPTT == 0x06))
                {
                    Return ("FPC1021")
                }

                Return ("DUMY0000")
            }

            Method (_INI, 0, NotSerialized)  
            {
                SHPO (GFPI, One)
                SHPO (GFPS, One)
            }

            Method (_STA, 0, NotSerialized)  
            {
                Return (Zero)
                If (((FPTT != Zero) && (SPIP == One)))
                {
                    Return (0x0F)
                }

                Return (Zero)
            }

            Method (_CRS, 0, Serialized)  
            {
                Name (BBUF, ResourceTemplate ()
                {
                    GpioIo (Exclusive, PullDefault, 0x0000, 0x0000, IoRestrictionOutputOnly,
                        "\\_SB.GPI0", 0x00, ResourceConsumer, ,
                        )
                        {   
                            0x0008
                        }
                })
                Name (IBUF, ResourceTemplate ()
                {
                    Interrupt (ResourceConsumer, Level, ActiveLow, ExclusiveAndWake, ,, _Y5B)
                    {
                        0x00000000,
                    }
                })
                Name (GBUF, ResourceTemplate ()
                {
                    GpioInt (Level, ActiveLow, ExclusiveAndWake, PullDefault, 0x0000,
                        "\\_SB.GPI0", 0x00, ResourceConsumer, _Y5C,
                        )
                        {   
                            0x0000
                        }
                })
                Name (UBUF, ResourceTemplate ()
                {
                    GpioIo (Exclusive, PullDefault, 0x0000, 0x0000, IoRestrictionInputOnly,
                        "\\_SB.GPI0", 0x00, ResourceConsumer, ,
                        )
                        {   
                            0x0000
                        }
                })
                CreateWordField (BBUF, 0x17, SPIN)
                CreateWordField (GBUF, 0x17, GPIN)
                CreateDWordField (IBUF, \_SB.PC00.SPI1.FPNT._CRS._Y5B._INT, IPIN)  
                CreateWordField (UBUF, 0x17, UPIN)
                CreateBitField (IBUF, \_SB.PC00.SPI1.FPNT._CRS._Y5B._LL, ILVL)  
                CreateBitField (IBUF, \_SB.PC00.SPI1.FPNT._CRS._Y5B._HE, ITRG)  
                CreateField (GBUF, \_SB.PC00.SPI1.FPNT._CRS._Y5C._POL, 0x02, GLVL)  
                CreateBitField (GBUF, \_SB.PC00.SPI1.FPNT._CRS._Y5C._MOD, GTRG)  
                SPIN = GNUM (GFPS)
                GPIN = GNUM (GFPI)
                IPIN = INUM (GFPI)
                UPIN = GNUM (GFPI)
                If (((FPTT == 0x02) || (FPTT == 0x06)))
                {
                    ILVL = Zero
                    ITRG = One
                    GLVL = Zero
                    GTRG = One
                }

                If ((FPTT == 0x04))
                {
                    ILVL = Zero
                    ITRG = One
                }

                Switch (ToInteger (FPTT))
                {
                    Case (One)
                    {
                        Local1 = 0x00989680
                        Local0 = Zero
                    }
                    Case (0x02)
                    {
                        Local1 = 0x002DC6C0
                        Local0 = Zero
                    }
                    Case (0x03)
                    {
                        Local1 = 0x007A1200
                        Local0 = One
                    }
                    Case (0x04)
                    {
                        Local1 = 0x007A1200
                        Local0 = Zero
                    }
                    Case (0x05)
                    {
                        Local1 = 0x00F42400
                        Local0 = Zero
                    }
                    Case (0x06)
                    {
                        Local1 = 0x002DC6C0
                        Local0 = Zero
                    }
                    Default
                    {
                        Local1 = Zero
                        Local0 = Zero
                    }

                }

                If ((FPTT == One))
                {
                    Return (ConcatenateResTemplate (SPIM (SPIX, Local0, Local1), BBUF))
                }

                If (((FPTT == 0x04) && (FPTM == Zero)))
                {
                    Return (ConcatenateResTemplate (ConcatenateResTemplate (SPIM (SPIX, Local0, Local1), BBUF), ConcatenateResTemplate (UBUF, GBUF
                        )))
                }

                If (((FPTT == 0x04) && (FPTM != Zero)))
                {
                    Return (ConcatenateResTemplate (ConcatenateResTemplate (SPIM (SPIX, Local0, Local1), BBUF), ConcatenateResTemplate (UBUF, IBUF
                        )))
                }

                If ((FPTM == Zero))
                {
                    Return (ConcatenateResTemplate (ConcatenateResTemplate (SPIM (SPIX, Local0, Local1), BBUF), GBUF))
                }

                Return (ConcatenateResTemplate (ConcatenateResTemplate (SPIM (SPIX, Local0, Local1), BBUF), IBUF))
            }
        }

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
