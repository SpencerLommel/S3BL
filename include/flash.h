#pragma once

// IMXRT1062 Flash Configuration Block
typedef struct {
    volatile uint32_t MCR0;
    volatile uint32_t MCR1;
    volatile uint32_t MCR2;
    volatile uint32_t AHBCR;
    volatile uint32_t INTEN;
    volatile uint32_t INTR;
    volatile uint32_t LUTKEY;
    volatile uint32_t LUTCR;
    volatile uint32_t AHBRXBUF0CR0;
    volatile uint32_t AHBRXBUF1CR0;
    volatile uint32_t AHBRXBUF2CR0;
    volatile uint32_t AHBRXBUF3CR0;
    volatile uint32_t FLSHCR0[4];
    volatile uint32_t FLSHCR1[4];
    volatile uint32_t FLSHCR2[4];
    volatile uint32_t FLSHCR4;
    volatile uint32_t IPCR0;
    volatile uint32_t IPCR1;
    volatile uint32_t IPCMD;
    volatile uint32_t DLPR;
    volatile uint32_t IPRXFCR;
    volatile uint32_t IPTXFCR;
    volatile uint32_t DLLACR;
    volatile uint32_t DLLBCR;
    volatile uint32_t STS0;
    volatile uint32_t STS1;
    volatile uint32_t STS2;
    volatile uint32_t AHBSPNDSTS;
    volatile uint32_t IPRXFSTS;
    volatile uint32_t IPTXFSTS;
    volatile uint32_t RFDR[32];
    volatile uint32_t TFDR[32];
    volatile uint32_t LUT[64];
} FLEX_RUNTIME_CFG_t;

#define IMXRT_FLEXSPI ((FLEX_RUNTIME_CFG_t *)0x402A8000)
#define FLEXSPI_LUT_KEY     0x5AF05AF0
#define FLEXSPI_LUT_UNLOCK  0x2
#define SECTOR_SIZE         4096