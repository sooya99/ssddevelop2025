//////////////////////////////////////////////////////////////////////////////////
// nvme_io_cmd.c for Cosmos+ OpenSSD  (Hardened Version)
// - Fixes: crash on zero-size namespace / invalid LBA by completing early
// - Keeps original function signatures and include paths
//////////////////////////////////////////////////////////////////////////////////

/*
#include "printf.h"
*/
#include <stdio.h>  /* jy */
#include "debug.h"
#include "io_access.h"

#include "nvme.h"
#include "host_lld.h"
#include "nvme_io_cmd.h"

#include "../ftl_config.h"
#include "../request_transform.h"

// -----------------------------------------------------------------------------
// Optional status codes (define if not provided by your headers)
#ifndef NVME_SC_SUCCESS
#define NVME_SC_SUCCESS 0x0
#endif
#ifndef NVME_SC_LBA_RANGE
#define NVME_SC_LBA_RANGE 0x80   // Generic "LBA out of range"
#endif
#ifndef NVME_SC_INVALID_FIELD
#define NVME_SC_INVALID_FIELD 0x2 // "Invalid field in command"
#endif
// -----------------------------------------------------------------------------

static inline void complete_nvme(unsigned int cmdSlotTag, unsigned int status)
{
    NVME_COMPLETION cpl;
    cpl.dword[0] = 0;
    cpl.specific = 0x0;
    cpl.statusFieldWord = status; // 0: success
    set_auto_nvme_cpl(cmdSlotTag, cpl.specific, cpl.statusFieldWord);
}

static inline int check_prp_aligned(const NVME_IO_COMMAND *cmd, int is_write)
{
    // Keep original alignment rules used in this codebase
    if (is_write) {
        if ((cmd->PRP1[0] & 0xF) != 0 || (cmd->PRP2[0] & 0xF) != 0) return 0;
    } else {
        if ((cmd->PRP1[0] & 0x3) != 0 || (cmd->PRP2[0] & 0x3) != 0) return 0;
    }
    if (cmd->PRP1[1] >= 0x10000 || cmd->PRP2[1] >= 0x10000) return 0;
    return 1;
}

static inline unsigned int ns_size(void)
{
    // Namespace size for this simulator equals total 4KB blocks (low dword)
    return storageCapacity_L;
}

// ------------------------------ READ -----------------------------------------
void handle_nvme_io_read(unsigned int cmdSlotTag, NVME_IO_COMMAND *nvmeIOCmd)
{
    IO_READ_COMMAND_DW12 readInfo12;
    unsigned int startLba[2];
    unsigned int nlb_zb; // zero-based NLB from DW12
    unsigned int nsze = ns_size();

    readInfo12.dword = nvmeIOCmd->dword[12];
    startLba[0] = nvmeIOCmd->dword[10];
    startLba[1] = nvmeIOCmd->dword[11];
    nlb_zb      = readInfo12.NLB; // 0 means 1 block

    // 1) If namespace size is 0 (partition not configured), don't crash
    if (nsze == 0) {
        printf("[NVMe][READ] namespace size is 0 (partition not set); early-complete.\n");
        complete_nvme(cmdSlotTag, NVME_SC_SUCCESS);
        return;
    }

    // 2) PRP alignment check (drop with error completion if invalid)
    if (!check_prp_aligned(nvmeIOCmd, 0)) {
        complete_nvme(cmdSlotTag, NVME_SC_INVALID_FIELD);
        return;
    }

    // 3) Upper 32-bit of SLBA must be zero in this model
    if (startLba[1] != 0) {
        complete_nvme(cmdSlotTag, NVME_SC_INVALID_FIELD);
        return;
    }

    // 4) Range check
    unsigned int xfer_blks = (nlb_zb == 0) ? 1 : (nlb_zb + 1);
    if (startLba[0] >= nsze || startLba[0] + xfer_blks > nsze) {
        complete_nvme(cmdSlotTag, NVME_SC_LBA_RANGE);
        return;
    }

    // 5) Valid → transform to slice requests
    ReqTransNvmeToSlice(cmdSlotTag, startLba[0], nlb_zb, IO_NVM_READ);
}

// ------------------------------ WRITE ----------------------------------------
void handle_nvme_io_write(unsigned int cmdSlotTag, NVME_IO_COMMAND *nvmeIOCmd)
{
    IO_READ_COMMAND_DW12 writeInfo12;
    unsigned int startLba[2];
    unsigned int nlb_zb;
    unsigned int nsze = ns_size();

    writeInfo12.dword = nvmeIOCmd->dword[12];
    startLba[0] = nvmeIOCmd->dword[10];
    startLba[1] = nvmeIOCmd->dword[11];
    nlb_zb      = writeInfo12.NLB;

    if (nsze == 0) {
        printf("[NVMe][WRITE] namespace size is 0 (partition not set); early-complete.\n");
        complete_nvme(cmdSlotTag, NVME_SC_SUCCESS);
        return;
    }

    if (!check_prp_aligned(nvmeIOCmd, 1)) {
        complete_nvme(cmdSlotTag, NVME_SC_INVALID_FIELD);
        return;
    }

    if (startLba[1] != 0) {
        complete_nvme(cmdSlotTag, NVME_SC_INVALID_FIELD);
        return;
    }

    unsigned int xfer_blks = (nlb_zb == 0) ? 1 : (nlb_zb + 1);
    if (startLba[0] >= nsze || startLba[0] + xfer_blks > nsze) {
        complete_nvme(cmdSlotTag, NVME_SC_LBA_RANGE);
        return;
    }

    ReqTransNvmeToSlice(cmdSlotTag, startLba[0], nlb_zb, IO_NVM_WRITE);
}

// ------------------------------ DISPATCH -------------------------------------
void handle_nvme_io_cmd(NVME_COMMAND *nvmeCmd)
{
    NVME_IO_COMMAND *nvmeIOCmd;
    unsigned int opc;
    nvmeIOCmd = (NVME_IO_COMMAND*)nvmeCmd->cmdDword;

    opc = (unsigned int)nvmeIOCmd->OPC;

    switch(opc)
    {
        case IO_NVM_FLUSH:
        {
            complete_nvme(nvmeCmd->cmdSlotTag, NVME_SC_SUCCESS);
            break;
        }
        case IO_NVM_WRITE:
        {
            handle_nvme_io_write(nvmeCmd->cmdSlotTag, nvmeIOCmd);
            break;
        }
        case IO_NVM_READ:
        {
            handle_nvme_io_read(nvmeCmd->cmdSlotTag, nvmeIOCmd);
            break;
        }
        default:
        {
            printf("Not Support IO Command OPC: %X\r\n", opc);
            complete_nvme(nvmeCmd->cmdSlotTag, NVME_SC_INVALID_FIELD);
            break;
        }
    }
}
