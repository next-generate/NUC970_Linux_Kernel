/*
 * arch/arm/mach-nuc970/include/mach/regs-sc.h
 *
 * Copyright (c) 2015 Nuvoton Technology Corporation
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifndef __ASM_ARCH_REGS_SC_H
#define __ASM_ARCH_REGS_SC_H

/* SC Registers */

#define SC0_BA			NUC970_VA_SC
#define SC1_BA			(NUC970_VA_SC + 0x400)
#define REG_SC_DAT		0x00
#define REG_SC_CTL		0x04
#define REG_SC_ALTCTL		0x08
#define REG_SC_EGT		0x0C
#define REG_SC_RXTOUT		0x10
#define REG_SC_ETUCTL		0x14
#define REG_SC_INTEN		0x18
#define REG_SC_INTSTS		0x1C
#define REG_SC_STATUS		0x20
#define REG_SC_PINCTL		0x24
#define REG_SC_TMRCTL0		0x28
#define REG_SC_TMRCTL1		0x2C
#define REG_SC_TMRCTL2		0x30
#define REG_SC_UARTCTL		0x34

#define SC_CTL_SYNC		0x40000000
#define SC_CTL_CDLV		0x04000000
#define SC_CTL_TXRTYEN		0x00800000
#define SC_CTL_TXRTY		0x00700000
#define SC_CTL_RXRTYEN		0x00080000
#define SC_CTL_RXRTY		0x00070000
#define SC_CTL_NSB		0x00008000
#define SC_CTL_TMRSEL		0x00006000
#define SC_CTL_AUTOCEN		0x00000008
#define SC_CTL_TXOFF		0x00000004
#define SC_CTL_RXOFF		0x00000002
#define SC_CTL_SCEN		0x00000001

#define SC_ALTCTL_ADACEN	0x00000800
#define SC_ALTCTL_CNTEN2	0x00000080
#define SC_ALTCTL_CNTEN1	0x00000040
#define SC_ALTCTL_CNTEN0	0x00000020
#define SC_ALTCTL_WARSTEN	0x00000010
#define SC_ALTCTL_ACTEN		0x00000008
#define SC_ALTCTL_DACTEN	0x00000004
#define SC_ALTCTL_RXRST		0x00000002
#define SC_ALTCTL_TXRST		0x00000001

#define SC_INTEN_ACERRIEN	0x00000400
#define SC_INTEN_RXTOIEN	0x00000200
#define SC_INTEN_INITIEN	0x00000100
#define SC_INTEN_CDIEN		0x00000080
#define SC_INTEN_TMR2IEN	0x00000020
#define SC_INTEN_TMR1IEN	0x00000010
#define SC_INTEN_TMR0IEN	0x00000008
#define SC_INTEN_TERRIEN	0x00000004
#define SC_INTEN_TBEIEN		0x00000002
#define SC_INTEN_RDAIEN		0x00000001

#define SC_INTSTS_ACERRIF	0x00000400
#define SC_INTSTS_RXTOIF	0x00000200
#define SC_INTSTS_INITIF	0x00000100
#define SC_INTSTS_CDIF		0x00000080
#define SC_INTSTS_TMR2IF	0x00000020
#define SC_INTSTS_TMR1IF	0x00000010
#define SC_INTSTS_TMR0IF	0x00000008
#define SC_INTSTS_TERRIF	0x00000004
#define SC_INTSTS_TBEIF		0x00000002
#define SC_INTSTS_RDAIF		0x00000001

#define SC_STATUS_TXOVERR	0x40000000
#define SC_STATUS_TXRERR	0x20000000
#define SC_STATUS_RXOVERR	0x00400000
#define SC_STATUS_RXRERR	0x00200000
#define SC_STATUS_CDPINSTS	0x00002000
#define SC_STATUS_CINSERT	0x00001000
#define SC_STATUS_CREMOVE	0x00000800
#define SC_STATUS_TXFULL	0x00000400
#define SC_STATUS_TXEMPTY	0x00000200
#define SC_STATUS_BEF		0x00000040
#define SC_STATUS_FEF		0x00000020
#define SC_STATUS_PEF		0x00000010
#define SC_STATUS_RXFULL	0x00000004
#define SC_STATUS_RXEMPTY	0x00000002
#define SC_STATUS_RXOV		0x00000001

#define SC_PINCTL_SYNC		0x40000000
#define SC_PINCTL_PWRINV	0x00000800
#define SC_PINCTL_SCDOUT	0x00000200
#define SC_PINCTL_CLKKEEP	0x00000040
#define SC_PINCTL_SCRST		0x00000002
#define SC_PINCTL_PWREN		0x00000001

#endif /*  __ASM_ARCH_REGS_SC_H */