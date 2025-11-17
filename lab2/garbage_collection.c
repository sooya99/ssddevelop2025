//////////////////////////////////////////////////////////////////////////////////
// garbage_collection.c for Cosmos+ OpenSSD
// Copyright (c) 2017 Hanyang University ENC Lab.
// Contributed by Yong Ho Song <yhsong@enc.hanyang.ac.kr>
//				  Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
//
// This file is part of Cosmos+ OpenSSD.
//
// Cosmos+ OpenSSD is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// Cosmos+ OpenSSD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Cosmos+ OpenSSD; see the file COPYING.
// If not, see <http://www.gnu.org/licenses/>.
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Company: ENC Lab. <http://enc.hanyang.ac.kr>
// Engineer: Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
//
// Project Name: Cosmos+ OpenSSD
// Design Name: Cosmos+ Firmware
// Module Name: Garbage Collector
// File Name: garbage_collection.c
//
// Version: v1.0.0
//
// Description:
//   - select a victim block
//   - collect valid pages to a free block
//   - erase a victim block to make a free block
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Revision History:
//
// * v1.0.0
//   - First draft
//////////////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdint.h>
#include "sim_backend.h"
#include <assert.h>
#include "memory_map.h"

/* externs for Cost-Benefit age */
extern unsigned int g_cb_tick;
extern unsigned int g_last_update_tick[USER_DIES][USER_BLOCKS_PER_DIE];

P_GC_VICTIM_MAP gcVictimMapPtr;

/* ===== GC-side summary counters (exported) ===== */
uint64_t g_ts_gc_victim_selects = 0;
uint64_t g_ts_gc_valid_copied_sum = 0;

/* address_translation.c (final epilogue)가 이 함수로 집계를 읽어감 */
void TsGcGetSummary(uint64_t *sel, uint64_t *sum)
{
    if (sel) *sel = g_ts_gc_victim_selects;
    if (sum) *sum = g_ts_gc_valid_copied_sum;
}

void InitGcVictimMap()
{
    int dieNo, invalidSliceCnt;

    gcVictimMapPtr = Addr2Mem(GC_VICTIM_MAP, GC_VICTIM_MAP_ADDR);

    for(dieNo=0 ; dieNo<USER_DIES; dieNo++)
    {
        for(invalidSliceCnt=0 ; invalidSliceCnt<SLICES_PER_BLOCK+1; invalidSliceCnt++)
        {
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
            gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
        }
    }
}

/* ---------------- Cost–Benefit victim selection (fixed) ----------------
 * 기존 코드: score = invalid / (eraseCnt + 1)
 * 문제: wear-levelling 편향으로 invalid가 적은 "젊은" 블록을 골라 live copy를 늘림.
 * 수정: score = invalid / (valid + 1)  (valid = SLICES_PER_BLOCK - invalid)
 *  - live copy를 최소화하는 방향의 점수.
 *  - tie-breaker: invalid 많음 > valid 적음 > eraseCnt 많음(균등 wear) > blockNo 낮음
 */
static unsigned int PickVictim_CostBenefit(unsigned int dieNo)
{
    unsigned int bestBlk = BLOCK_FAIL;
    double bestScore = -1.0;
    unsigned int blk;

    for (blk = 0; blk < USER_BLOCKS_PER_DIE; blk++)
    {
        if (virtualBlockMapPtr->block[dieNo][blk].bad)  continue;           // skip bad
        if (virtualBlockMapPtr->block[dieNo][blk].free) continue;           // skip free

        // active write block은 피해간다 (안전 장치: BLOCK_NONE일 땐 스킵 안 함)
        if (virtualDieMapPtr->die[dieNo].currentBlock != BLOCK_NONE &&
            blk == virtualDieMapPtr->die[dieNo].currentBlock) continue;

        unsigned int inv = virtualBlockMapPtr->block[dieNo][blk].invalidSliceCnt;
        if (inv == 0) continue;

        unsigned int valid = SLICES_PER_BLOCK - inv;
        unsigned int er    = virtualBlockMapPtr->block[dieNo][blk].eraseCnt;

        // cost-benefit: 더 많은 invalid, 더 적은 valid → 더 큰 score
                unsigned int age = g_cb_tick - g_last_update_tick[dieNo][blk];
        double score;
        if (valid == 0) {
            score = (double)age; // all invalid, best candidate
        } else {
            score = (double)age * ((double)inv) / (2.0 * (double)valid);
        }

        if (score > bestScore) {
            bestScore = score;
            bestBlk = blk;
        } else if (score == bestScore && bestBlk != BLOCK_FAIL) {
            // tie-breakers
            unsigned int invBest   = virtualBlockMapPtr->block[dieNo][bestBlk].invalidSliceCnt;
            unsigned int validBest = SLICES_PER_BLOCK - invBest;
            unsigned int erBest    = virtualBlockMapPtr->block[dieNo][bestBlk].eraseCnt;
            if (inv > invBest ||
                (inv == invBest && valid < validBest) ||
                (inv == invBest && valid == validBest && er > erBest) ||
                (inv == invBest && valid == validBest && er == erBest && blk < bestBlk)) {
                bestBlk = blk;
            }
        }
    }

    if (bestBlk != BLOCK_FAIL) {
        /* keep bucket list integrity */
        SelectiveGetFromGcVictimList(dieNo, bestBlk);
        g_ts_gc_victim_selects++;
        return bestBlk;
    }
    return BLOCK_FAIL;
}

void GarbageCollection(unsigned int dieNo)
{
    unsigned int __validCopied = 0;
    unsigned int victimBlockNo, pageNo, virtualSliceAddr, logicalSliceAddr, dieNoForGcCopy, reqSlotTag;

    victimBlockNo = PickVictim_CostBenefit(dieNo);
    assert(victimBlockNo != BLOCK_FAIL && "[WARNING] No GC victim block [WARNING]");
    dieNoForGcCopy = dieNo;

    if(virtualBlockMapPtr->block[dieNo][victimBlockNo].invalidSliceCnt != SLICES_PER_BLOCK)
    {
        for(pageNo=0 ; pageNo<USER_PAGES_PER_BLOCK ; pageNo++)
        {
            virtualSliceAddr = Vorg2VsaTranslation(dieNo, victimBlockNo, pageNo);
            logicalSliceAddr = virtualSliceMapPtr->virtualSlice[virtualSliceAddr].logicalSliceAddr;

            if(logicalSliceAddr != LSA_NONE)
            if(logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr ==  virtualSliceAddr)
            {
                /* read */
                reqSlotTag = GetFromFreeReqQ();

                reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
                reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_READ;
                reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = logicalSliceAddr;
                reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
                reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
                reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
                reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
                reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
                reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
                reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = AllocateTempDataBuf(dieNo);
                UpdateTempDataBufEntryInfoBlockingReq(reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry, reqSlotTag);
                reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = virtualSliceAddr;

                SelectLowLevelReqQ(reqSlotTag);

                /* write */
                reqSlotTag = GetFromFreeReqQ();

                reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
                reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_WRITE;
                reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = logicalSliceAddr;
                reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
                reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
                reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
                reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
                reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
                reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
                reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = AllocateTempDataBuf(dieNo);
                UpdateTempDataBufEntryInfoBlockingReq(reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry, reqSlotTag);
                reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = FindFreeVirtualSliceForGc(dieNoForGcCopy, victimBlockNo);

                logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr = reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr;
                virtualSliceMapPtr->virtualSlice[reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr].logicalSliceAddr = logicalSliceAddr;

                SelectLowLevelReqQ(reqSlotTag);

                /* count valid page copied */
                __validCopied++;
            }
        }
    }

    g_ts_gc_valid_copied_sum += __validCopied;
    EraseBlock(dieNo, victimBlockNo);
}

void PutToGcVictimList(unsigned int dieNo, unsigned int blockNo, unsigned int invalidSliceCnt)
{
    if(gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock != BLOCK_NONE)
    {
        virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock;
        virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
        virtualBlockMapPtr->block[dieNo][gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock].nextBlock = blockNo;
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = blockNo;
    }
    else
    {
        virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = BLOCK_NONE;
        virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = blockNo;
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = blockNo;
    }
}

unsigned int GetFromGcVictimList(unsigned int dieNo)
{
    /* In CB mode, select by PickVictim_CostBenefit. */
    return PickVictim_CostBenefit(dieNo);
}

void SelectiveGetFromGcVictimList(unsigned int dieNo, unsigned int blockNo)
{
    unsigned int nextBlock, prevBlock, invalidSliceCnt;

    nextBlock = virtualBlockMapPtr->block[dieNo][blockNo].nextBlock;
    prevBlock = virtualBlockMapPtr->block[dieNo][blockNo].prevBlock;
    invalidSliceCnt = virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt;

    if((nextBlock != BLOCK_NONE) && (prevBlock != BLOCK_NONE))
    {
        virtualBlockMapPtr->block[dieNo][prevBlock].nextBlock = nextBlock;
        virtualBlockMapPtr->block[dieNo][nextBlock].prevBlock = prevBlock;
    }
    else if((nextBlock == BLOCK_NONE) && (prevBlock != BLOCK_NONE))
    {
        virtualBlockMapPtr->block[dieNo][prevBlock].nextBlock = BLOCK_NONE;
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = prevBlock;
    }
    else if((nextBlock != BLOCK_NONE) && (prevBlock == BLOCK_NONE))
    {
        virtualBlockMapPtr->block[dieNo][nextBlock].prevBlock = BLOCK_NONE;
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = nextBlock;
    }
    else
    {
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
        gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
    }

    /* detach node from list */
    virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
    virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = BLOCK_NONE;
}

