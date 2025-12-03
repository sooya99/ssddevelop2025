//////////////////////////////////////////////////////////////////////////////////
// address_translation.c for Cosmos+ OpenSSD
// Copyright (c) 2017 Hanyang University ENC Lab.
// Contributed by Yong Ho Song <yhsong@enc.hanyang.ac.kr>
//				  Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
//				  Sangjin Lee <sjlee@enc.hanyang.ac.kr>
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
// Module Name: Address Translator
// File Name: address translation.c
//
// Version: v1.0.0
//
// Description:
//   - translate address between address space of host system and address space of NAND device
//   - manage bad blocks in NAND device
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Revision History:
//
// * v1.0.0
//   - First draft
//////////////////////////////////////////////////////////////////////////////////

#include <assert.h>
#include "memory_map.h"
#include "xil_printf.h"
#include <stdint.h>

/* ===== Cost-Benefit age tracking ===== */
unsigned int g_cb_tick = 0;
unsigned int g_last_update_tick[USER_DIES][USER_BLOCKS_PER_DIE];

/* ===== Per-block write-tracking for hot/cold classification ===== */
unsigned int g_block_write_cnt[USER_DIES][USER_BLOCKS_PER_DIE];
unsigned char g_block_is_hot[USER_DIES][USER_BLOCKS_PER_DIE];

/* ===== Workload Statistics Counters ===== */
uint64_t g_stat_host_write_count = 0;      // Host logical page writes
uint64_t g_stat_nand_program_count = 0;    // Total NAND page programs (host + GC)

/* ===== FTL Summary counters (final report only) ===== */
static uint64_t ts_total_writes = 0;
static uint64_t ts_total_invalidates = 0;
static uint64_t ts_total_erases = 0;

/* from GC */
extern void TsGcGetSummary(uint64_t *gc_selects, uint64_t *gc_valid_sum);

static void TsDumpSummary(void)
{
    uint64_t gc_selects = 0, gc_valid_sum = 0;
    TsGcGetSummary(&gc_selects, &gc_valid_sum);

    xil_printf("\n==== FTL Timestamp Summary ====\n");
    xil_printf("Writes (LSA->VSA)   : %llu\n", (unsigned long long)ts_total_writes);
    xil_printf("Invalidations (VSA) : %llu\n", (unsigned long long)ts_total_invalidates);
    xil_printf("Erases (blocks)     : %llu\n", (unsigned long long)ts_total_erases);
    xil_printf("GC victim selects   : %llu\n", (unsigned long long)gc_selects);
    xil_printf("GC valid copied sum : %llu\n", (unsigned long long)gc_valid_sum);
    xil_printf("==== End of Summary ====\n");
}

/* Print workload statistics */
void PrintWorkloadStatistics(void)
{
    unsigned int die, blk;
    unsigned long long total_block_writes = 0;
    unsigned int total_blocks = USER_DIES * USER_BLOCKS_PER_DIE;
    unsigned int min_writes = 0xFFFFFFFF;
    unsigned int max_writes = 0;

    /* GC timestamp-based summary */
    uint64_t gc_selects = 0;
    uint64_t gc_valid_sum = 0;
    TsGcGetSummary(&gc_selects, &gc_valid_sum);

    /* 블록별 write 수 집계 */
    for (die = 0; die < USER_DIES; die++) {
        for (blk = 0; blk < USER_BLOCKS_PER_DIE; blk++) {
            unsigned int cnt = g_block_write_cnt[die][blk];
            total_block_writes += cnt;
            if (cnt < min_writes) min_writes = cnt;
            if (cnt > max_writes) max_writes = cnt;
        }
    }

    xil_printf("\r\n========== Workload Statistics ==========\r\n");

    /* 호스트 논리 쓰기 (LSA 기준) */
    xil_printf("Host logical writes     : %u pages\r\n",
               (unsigned int)g_stat_host_write_count);

    /* (참고) 카운터 기반 NAND 프로그램 수 */
    xil_printf("NAND programs (counter) : %u pages\r\n",
               (unsigned int)g_stat_nand_program_count);

    /* GC timestamp 기반 통계 */
    xil_printf("GC victim selects       : %u\r\n",
               (unsigned int)gc_selects);
    xil_printf("GC valid pages copied   : %u\r\n",
               (unsigned int)gc_valid_sum);

    /* timestamp 기반으로 추정한 NAND 프로그램 수 & WAF 계산 */
    if (g_stat_host_write_count > 0) {
        /* 호스트 논리 쓰기 1회당 NAND program 1회라고 보고,
           GC valid copy 수를 추가로 더해 NAND program 추정치를 만든다. */
        uint64_t host_writes   = g_stat_host_write_count;
        uint64_t nand_prog_est = host_writes + gc_valid_sum;

        /* 64비트 정수 연산으로 WAF * 1000 계산 */
        uint64_t waf_x1000 = (nand_prog_est * 1000ULL) / host_writes;
        unsigned int waf_whole = (unsigned int)(waf_x1000 / 1000ULL);
        unsigned int waf_frac  = (unsigned int)(waf_x1000 % 1000ULL);

        xil_printf("NAND programs (est.)    : %u pages (host %u + GC %u)\r\n",
                   (unsigned int)nand_prog_est,
                   (unsigned int)host_writes,
                   (unsigned int)gc_valid_sum);

        xil_printf("WAF (est. NAND/host)    : %u.%03u\r\n",
                   waf_whole, waf_frac);
    } else {
        xil_printf("NAND programs (est.)    : 0 pages\r\n");
        xil_printf("WAF (est. NAND/host)    : N/A (no host writes)\r\n");
    }

    /* 블록 wear-leveling 지표 */
    xil_printf("Total block writes      : %u\r\n",
               (unsigned int)total_block_writes);

    if (total_blocks > 0 && total_block_writes > 0) {
        /* 평균 writes/block (소수 둘째 자리까지) */
        uint64_t avg_x100 = (total_block_writes * 100ULL) / total_blocks;
        unsigned int avg_whole = (unsigned int)(avg_x100 / 100ULL);
        unsigned int avg_frac  = (unsigned int)(avg_x100 % 100ULL);
        xil_printf("Average writes/block    : %u.%02u\r\n",
                   avg_whole, avg_frac);
    } else {
        xil_printf("Average writes/block    : 0.00\r\n");
    }

    xil_printf("Min writes/block        : %u\r\n",
               (min_writes == 0xFFFFFFFF ? 0 : min_writes));
    xil_printf("Max writes/block        : %u\r\n", max_writes);
    xil_printf("Current tick             : %u\r\n", g_cb_tick);
    xil_printf("=========================================\r\n\r\n");
}


P_LOGICAL_SLICE_MAP logicalSliceMapPtr;
P_VIRTUAL_SLICE_MAP virtualSliceMapPtr;
P_VIRTUAL_BLOCK_MAP virtualBlockMapPtr;
P_VIRTUAL_DIE_MAP virtualDieMapPtr;
P_PHY_BLOCK_MAP phyBlockMapPtr;
P_BAD_BLOCK_TABLE_INFO_MAP bbtInfoMapPtr;

unsigned char sliceAllocationTargetDie;
unsigned int mbPerbadBlockSpace;


void InitAddressMap()
{
	unsigned int blockNo, dieNo;

	logicalSliceMapPtr = (P_LOGICAL_SLICE_MAP ) LOGICAL_SLICE_MAP_ADDR;
	virtualSliceMapPtr = (P_VIRTUAL_SLICE_MAP) VIRTUAL_SLICE_MAP_ADDR;
	virtualBlockMapPtr = (P_VIRTUAL_BLOCK_MAP) VIRTUAL_BLOCK_MAP_ADDR;
	virtualDieMapPtr = (P_VIRTUAL_DIE_MAP) VIRTUAL_DIE_MAP_ADDR;
	phyBlockMapPtr = (P_PHY_BLOCK_MAP) PHY_BLOCK_MAP_ADDR;
	bbtInfoMapPtr = (P_BAD_BLOCK_TABLE_INFO_MAP) BAD_BLOCK_TABLE_INFO_MAP_ADDR;

	//init phyblockMap
	for(dieNo=0 ; dieNo<USER_DIES ; dieNo++)
	{
		for(blockNo=0 ; blockNo<TOTAL_BLOCKS_PER_DIE ; blockNo++)
			phyBlockMapPtr->phyBlock[dieNo][blockNo].remappedPhyBlock = blockNo;

		bbtInfoMapPtr->bbtInfo[dieNo].phyBlock = 0;
		bbtInfoMapPtr->bbtInfo[dieNo].grownBadUpdate = BBT_INFO_GROWN_BAD_UPDATE_NONE;
	}

	sliceAllocationTargetDie = FindDieForFreeSliceAllocation();

	InitSliceMap();
	InitBlockDieMap();
}

void InitSliceMap()
{
	int sliceAddr;
	for(sliceAddr=0; sliceAddr<SLICES_PER_SSD ; sliceAddr++)
	{
		logicalSliceMapPtr->logicalSlice[sliceAddr].virtualSliceAddr = VSA_NONE;
		virtualSliceMapPtr->virtualSlice[sliceAddr].logicalSliceAddr = LSA_NONE;
	}
}

void RemapBadBlock()
{
	unsigned int blockNo, dieNo, remapFlag, maxBadBlockCount;
	unsigned int reservedBlockOfLun0[USER_DIES];
	unsigned int reservedBlockOfLun1[USER_DIES];
	unsigned int badBlockCount[USER_DIES];

	xil_printf("Bad block remapping start...\r\n");

	for(dieNo=0 ; dieNo<USER_DIES ; dieNo++)
	{
		reservedBlockOfLun0[dieNo] = USER_BLOCKS_PER_LUN;
		reservedBlockOfLun1[dieNo] = TOTAL_BLOCKS_PER_LUN + USER_BLOCKS_PER_LUN;
		badBlockCount[dieNo] = 0;
	}


	for(blockNo=0 ; blockNo<USER_BLOCKS_PER_LUN ; blockNo++)
	{
		for(dieNo=0 ; dieNo<USER_DIES ; dieNo++)
		{
			//lun0
			if(phyBlockMapPtr->phyBlock[dieNo][blockNo].bad)
			{
				if(reservedBlockOfLun0[dieNo] < TOTAL_BLOCKS_PER_LUN)
				{
					remapFlag = 1;
					while(phyBlockMapPtr->phyBlock[dieNo][reservedBlockOfLun0[dieNo]].bad)
					{
						reservedBlockOfLun0[dieNo]++;
						if(reservedBlockOfLun0[dieNo] >= TOTAL_BLOCKS_PER_LUN)
						{
							remapFlag = 0;
							break;
						}
					}

					if(remapFlag)
					{
						phyBlockMapPtr->phyBlock[dieNo][blockNo].remappedPhyBlock  = reservedBlockOfLun0[dieNo];
						reservedBlockOfLun0[dieNo]++;
					}
					else
					{
						xil_printf("No reserved block - Ch %d Way %d virtualBlock %d is bad block \r\n", Vdie2PchTranslation(dieNo), Vdie2PwayTranslation(dieNo), blockNo);
						badBlockCount[dieNo]++;
					}
				}
				else
				{
					xil_printf("No reserved block - Ch %d Way %d virtualBlock %d is bad block \r\n", Vdie2PchTranslation(dieNo), Vdie2PwayTranslation(dieNo), blockNo);
					badBlockCount[dieNo]++;
				}
			}

			if (LUNS_PER_DIE > 1)
			{
				//lun1
				if(phyBlockMapPtr->phyBlock[dieNo][blockNo+TOTAL_BLOCKS_PER_LUN].bad)
				{
					if(reservedBlockOfLun1[dieNo] < TOTAL_BLOCKS_PER_DIE)
					{
						remapFlag = 1;
						while(phyBlockMapPtr->phyBlock[dieNo][reservedBlockOfLun1[dieNo]].bad)
						{
							reservedBlockOfLun1[dieNo]++;
							if(reservedBlockOfLun1[dieNo] >= TOTAL_BLOCKS_PER_DIE)
							{
								remapFlag = 0;
								break;
							}
						}

						if(remapFlag)
						{
							phyBlockMapPtr->phyBlock[dieNo][blockNo+TOTAL_BLOCKS_PER_LUN].remappedPhyBlock  = reservedBlockOfLun1[dieNo];
							reservedBlockOfLun1[dieNo]++;
						}
						else
						{
							xil_printf("No reserved block - Ch %x Way %x virtualBlock %d is bad block \r\n",  Vdie2PchTranslation(dieNo), Vdie2PwayTranslation(dieNo), blockNo+USER_BLOCKS_PER_LUN);
							badBlockCount[dieNo]++;
						}
					}
					else
					{
						xil_printf("No reserved block - Ch %x Way %x virtualBlock %d is bad block \r\n",  Vdie2PchTranslation(dieNo), Vdie2PwayTranslation(dieNo), blockNo+USER_BLOCKS_PER_LUN);
						badBlockCount[dieNo]++;
					}
				}
			}
		}
	}

	xil_printf("Bad block remapping end\r\n");


	maxBadBlockCount = 0;
	for(dieNo=0; dieNo < USER_DIES; dieNo++)
	{
		if(maxBadBlockCount < badBlockCount[dieNo])
			maxBadBlockCount = badBlockCount[dieNo];
	}

	mbPerbadBlockSpace = maxBadBlockCount * USER_DIES * MB_PER_BLOCK;
}

void InitDieMap()
{
	unsigned int dieNo;

	for(dieNo=0 ; dieNo<USER_DIES ; dieNo++)
	{
		virtualDieMapPtr->die[dieNo].headFreeBlock = BLOCK_NONE;
		virtualDieMapPtr->die[dieNo].tailFreeBlock = BLOCK_NONE;
		virtualDieMapPtr->die[dieNo].freeBlockCnt = 0;
	}
}

void InitBlockMap()
{
	unsigned int dieNo, phyBlockNo, virtualBlockNo, remappedPhyBlock;

	for(dieNo=0 ; dieNo<USER_DIES ; dieNo++)
	{
		for(virtualBlockNo=0; virtualBlockNo<USER_BLOCKS_PER_DIE ; virtualBlockNo++)
		{
			phyBlockNo = Vblock2PblockOfTbsTranslation(virtualBlockNo);
			remappedPhyBlock = phyBlockMapPtr->phyBlock[dieNo][phyBlockNo].remappedPhyBlock;
			virtualBlockMapPtr->block[dieNo][virtualBlockNo].bad = phyBlockMapPtr->phyBlock[dieNo][remappedPhyBlock].bad;

			virtualBlockMapPtr->block[dieNo][virtualBlockNo].free = 1;
			virtualBlockMapPtr->block[dieNo][virtualBlockNo].invalidSliceCnt = 0;
			virtualBlockMapPtr->block[dieNo][virtualBlockNo].currentPage = 0;
			virtualBlockMapPtr->block[dieNo][virtualBlockNo].eraseCnt = 0;

			g_last_update_tick[dieNo][virtualBlockNo] = 0;
			g_block_write_cnt[dieNo][virtualBlockNo] = 0;
			g_block_is_hot[dieNo][virtualBlockNo] = 0;

			if(virtualBlockMapPtr->block[dieNo][virtualBlockNo].bad)
			{
				virtualBlockMapPtr->block[dieNo][virtualBlockNo].prevBlock = BLOCK_NONE;
				virtualBlockMapPtr->block[dieNo][virtualBlockNo].nextBlock = BLOCK_NONE;
			}
			else
				PutToFbList(dieNo, virtualBlockNo);
		}
	}
}

void InitCurrentBlockOfDieMap()
{
	unsigned int dieNo;

	for(dieNo=0 ; dieNo<USER_DIES ; dieNo++)
	{
		virtualDieMapPtr->die[dieNo].currentBlock = GetFromFbList(dieNo, GET_FREE_BLOCK_NORMAL);
		if(virtualDieMapPtr->die[dieNo].currentBlock == BLOCK_FAIL)
			assert(!"[WARNING] There is no free block [WARNING]");
	}
}

void ReadBadBlockTable(unsigned int tempBbtBufAddr[], unsigned int tempBbtBufEntrySize)
{
	unsigned int tempPage, reqSlotTag, dieNo;
	int loop, dataSize;

	loop = 0;
	dataSize = DATA_SIZE_OF_BAD_BLOCK_TABLE_PER_DIE;
	tempPage = PlsbPage2VpageTranslation(START_PAGE_NO_OF_BAD_BLOCK_TABLE_BLOCK); 	//bad block table is saved at lsb pages

	while(dataSize>0)
	{
		for(dieNo = 0; dieNo < USER_DIES; dieNo++)
		{
			reqSlotTag = GetFromFreeReqQ();

			reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
			reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_READ;
			reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
			reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
			reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
			reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
			reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
			reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_TOTAL;

			reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.addr = tempBbtBufAddr[dieNo] + loop * tempBbtBufEntrySize;

			reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalCh = Vdie2PchTranslation(dieNo);
			reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalWay = Vdie2PwayTranslation(dieNo);
			reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalBlock = bbtInfoMapPtr->bbtInfo[dieNo].phyBlock;
			reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalPage = Vpage2PlsbPageTranslation(tempPage);

			SelectLowLevelReqQ(reqSlotTag);
		}

		tempPage++;
		loop++;
		dataSize -= BYTES_PER_DATA_REGION_OF_PAGE;
	}

	SyncAllLowLevelReqDone();
}

void FindBadBlock(unsigned char dieState[], unsigned int tempBbtBufAddr[], unsigned int tempBbtBufEntrySize, unsigned int tempReadBufAddr[], unsigned int tempReadBufEntrySize)
{
	unsigned int phyBlockNo, dieNo, reqSlotTag;
	unsigned char blockChecker[USER_DIES];
	unsigned char* markPointer0;
	unsigned char* markPointer1;
	unsigned char* bbtUpdater;

	//check bad block mark of each block
	for(phyBlockNo = 0; phyBlockNo < TOTAL_BLOCKS_PER_DIE; phyBlockNo++)
	{
		for(dieNo=0; dieNo < USER_DIES; dieNo++)
			if(!dieState[dieNo])
			{
				blockChecker[dieNo] = BLOCK_STATE_NORMAL;

				reqSlotTag = GetFromFreeReqQ();

				reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
				reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_READ;
				reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
				reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
				reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_OFF;
				reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
				reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
				reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_TOTAL;

				reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.addr = tempReadBufAddr[dieNo];

				reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalCh = Vdie2PchTranslation(dieNo);
				reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalWay = Vdie2PwayTranslation(dieNo);
				reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalBlock = phyBlockNo;
				reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalPage = BAD_BLOCK_MARK_PAGE0;

				SelectLowLevelReqQ(reqSlotTag);
			}

		SyncAllLowLevelReqDone();

		for(dieNo=0; dieNo < USER_DIES; dieNo++)
			if(!dieState[dieNo])
			{
				markPointer0 = (unsigned char*)(tempReadBufAddr[dieNo] + BAD_BLOCK_MARK_BYTE0);
				markPointer1 = (unsigned char*)(tempReadBufAddr[dieNo] + BAD_BLOCK_MARK_BYTE1);

				if((*markPointer0 == CLEAN_DATA_IN_BYTE) && (*markPointer1 == CLEAN_DATA_IN_BYTE))
				{
					reqSlotTag = GetFromFreeReqQ();

					reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
					reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_READ;
					reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
					reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
					reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_OFF;
					reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
					reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
					reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_TOTAL;

					reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.addr = tempReadBufAddr[dieNo];

					reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalCh = Vdie2PchTranslation(dieNo);
					reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalWay = Vdie2PwayTranslation(dieNo);
					reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalBlock = phyBlockNo;
					reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalPage = BAD_BLOCK_MARK_PAGE1;

					SelectLowLevelReqQ(reqSlotTag);
				}
				else
				{
					xil_printf("	bad block is detected: Ch %d Way %d phyBlock %d \r\n",Vdie2PchTranslation(dieNo), Vdie2PwayTranslation(dieNo), phyBlockNo);

					blockChecker[dieNo] = BLOCK_STATE_BAD;
				}
			}

		SyncAllLowLevelReqDone();

		for(dieNo=0; dieNo < USER_DIES; dieNo++)
			if(!dieState[dieNo])
			{
				markPointer0 = (unsigned char*)(tempReadBufAddr[dieNo] + BAD_BLOCK_MARK_BYTE0);
				markPointer1 = (unsigned char*)(tempReadBufAddr[dieNo] + BAD_BLOCK_MARK_BYTE1);

				if(!((*markPointer0 == CLEAN_DATA_IN_BYTE) && (*markPointer1 == CLEAN_DATA_IN_BYTE)))
					if(blockChecker[dieNo] == BLOCK_STATE_NORMAL)
					{
						xil_printf("	bad block is detected: Ch %d Way %d phyBlock %d \r\n",Vdie2PchTranslation(dieNo), Vdie2PwayTranslation(dieNo), phyBlockNo);

						blockChecker[dieNo] = BLOCK_STATE_BAD;

					}

				bbtUpdater= (unsigned char*)(tempBbtBufAddr[dieNo] + phyBlockNo);
				*bbtUpdater = blockChecker[dieNo];
				phyBlockMapPtr->phyBlock[dieNo][phyBlockNo].bad = blockChecker[dieNo];
			}
	}
}


void SaveBadBlockTable(unsigned char dieState[], unsigned int tempBbtBufAddr[], unsigned int tempBbtBufEntrySize)
{
	unsigned int dieNo, reqSlotTag;
	int loop, dataSize, tempPage;

	loop = 0;
	dataSize = DATA_SIZE_OF_BAD_BLOCK_TABLE_PER_DIE;
	tempPage = PlsbPage2VpageTranslation(START_PAGE_NO_OF_BAD_BLOCK_TABLE_BLOCK);	//bad block table is saved at lsb pages

	while(dataSize>0)
	{
		for(dieNo = 0; dieNo < USER_DIES; dieNo++)
			if((dieState[dieNo] == DIE_STATE_BAD_BLOCK_TABLE_NOT_EXIST) || (dieState[dieNo] == DIE_STATE_BAD_BLOCK_TABLE_UPDATE))
			{
				if(loop == 0)
				{
					reqSlotTag = GetFromFreeReqQ();

					reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
					reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_ERASE;
					reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
					reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
					reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
					reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_TOTAL;

					reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalCh = Vdie2PchTranslation(dieNo);
					reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalWay = Vdie2PwayTranslation(dieNo);
					reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalBlock = bbtInfoMapPtr->bbtInfo[dieNo].phyBlock;
					reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalPage = 0;	//dummy

					SelectLowLevelReqQ(reqSlotTag);
				}

				reqSlotTag = GetFromFreeReqQ();

				reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
				reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_WRITE;
				reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_ADDR;
				reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
				reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
				reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
				reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
				reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_TOTAL;

				reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.addr = tempBbtBufAddr[dieNo] + loop * tempBbtBufEntrySize;

				reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalCh = Vdie2PchTranslation(dieNo);
				reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalWay = Vdie2PwayTranslation(dieNo);
				reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalBlock = bbtInfoMapPtr->bbtInfo[dieNo].phyBlock;
				reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalPage =  Vpage2PlsbPageTranslation(tempPage);

				SelectLowLevelReqQ(reqSlotTag);
			}

		loop++;
		dataSize++;
		dataSize -= BYTES_PER_DATA_REGION_OF_PAGE;
	}

	SyncAllLowLevelReqDone();


	for(dieNo=0; dieNo < USER_DIES; dieNo++)
		if(dieState[dieNo] == DIE_STATE_BAD_BLOCK_TABLE_NOT_EXIST)
			xil_printf("[ bad block table of Ch %d Way %d is saved. ]\r\n", dieNo%USER_CHANNELS, dieNo/USER_CHANNELS);
}


void RecoverBadBlockTable(unsigned int tempBufAddr)
{
	unsigned int dieNo, phyBlockNo, bbtMaker, tempBbtBufBaseAddr, tempBbtBufEntrySize, tempReadBufBaseAddr, tempReadBufEntrySize;
	unsigned int tempBbtBufAddr[USER_DIES];
	unsigned int tempReadBufAddr[USER_DIES];
	unsigned char dieState[USER_DIES];
	unsigned char* bbtTableChecker;

	//data buffer allocation
	tempBbtBufBaseAddr = tempBufAddr;
	tempBbtBufEntrySize = BYTES_PER_DATA_REGION_OF_PAGE + BYTES_PER_SPARE_REGION_OF_PAGE;
	tempReadBufBaseAddr = tempBbtBufBaseAddr + USER_DIES * USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE * tempBbtBufEntrySize;
	tempReadBufEntrySize = BYTES_PER_NAND_ROW;
	for(dieNo = 0; dieNo < USER_DIES; dieNo++)
	{
		tempBbtBufAddr[dieNo] = tempBbtBufBaseAddr + dieNo * USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE * tempBbtBufEntrySize;
		tempReadBufAddr[dieNo] = tempReadBufBaseAddr + dieNo * tempReadBufEntrySize;
	}

	//read bad block tables
	ReadBadBlockTable(tempBbtBufAddr, tempBbtBufEntrySize);

	//check bad block tables
	bbtMaker = BAD_BLOCK_TABLE_MAKER_IDLE;
	for(dieNo=0; dieNo<USER_DIES; dieNo++)
	{
		bbtTableChecker = (unsigned char*)(tempBbtBufAddr[dieNo]);

		if((*bbtTableChecker == BLOCK_STATE_NORMAL)||(*bbtTableChecker == BLOCK_STATE_BAD))
		{
			xil_printf("[ bad block table of ch %d way %d exists.]\r\n",Vdie2PchTranslation(dieNo), Vdie2PwayTranslation(dieNo));

			dieState[dieNo] = DIE_STATE_BAD_BLOCK_TABLE_EXIST;
			for(phyBlockNo=0; phyBlockNo<TOTAL_BLOCKS_PER_DIE; phyBlockNo++)
			{
				bbtTableChecker = (unsigned char*)(tempBbtBufAddr[dieNo] + phyBlockNo);

				phyBlockMapPtr->phyBlock[dieNo][phyBlockNo].bad = *bbtTableChecker;
				if(phyBlockMapPtr->phyBlock[dieNo][phyBlockNo].bad == BLOCK_STATE_BAD)
					xil_printf("	bad block: ch %d way %d phyBlock %d  \r\n", Vdie2PchTranslation(dieNo), Vdie2PwayTranslation(dieNo), phyBlockNo);
			}

			xil_printf("[ bad blocks of ch %d way %d are checked. ]\r\n",Vdie2PchTranslation(dieNo), Vdie2PwayTranslation(dieNo));
		}
		else
		{
			xil_printf("[ bad block table of ch %d way %d does not exist.]\r\n",Vdie2PchTranslation(dieNo), Vdie2PwayTranslation(dieNo));
			dieState[dieNo] = DIE_STATE_BAD_BLOCK_TABLE_NOT_EXIST;
			bbtMaker = BAD_BLOCK_TABLE_MAKER_TRIGGER;
		}
	}

	//if bad block table does not exist in some dies, make new bad block table for each die having no bad block table
	if(bbtMaker == BAD_BLOCK_TABLE_MAKER_TRIGGER)
	{
		FindBadBlock(dieState, tempBbtBufAddr, tempBbtBufEntrySize, tempReadBufAddr, tempReadBufEntrySize);
		SaveBadBlockTable(dieState, tempBbtBufAddr, tempBbtBufEntrySize);
	}

	//grown bad update flag initialization
	for(dieNo=0; dieNo<USER_DIES; dieNo++)
		bbtInfoMapPtr->bbtInfo[dieNo].grownBadUpdate = BBT_INFO_GROWN_BAD_UPDATE_NONE;
}



void EraseTotalBlockSpace()
{
	unsigned int blockNo, dieNo, reqSlotTag;

	xil_printf("Erase total block space...wait for a minute...\r\n");

	for(blockNo=0 ; blockNo<TOTAL_BLOCKS_PER_DIE ; blockNo++)
		for(dieNo=0 ; dieNo<USER_DIES ; dieNo++)
		{
			reqSlotTag = GetFromFreeReqQ();

			reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
			reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_ERASE;
			reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_PHY_ORG;
			reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
			reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
			reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_TOTAL;

			reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalCh = Vdie2PchTranslation(dieNo);
			reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalWay = Vdie2PwayTranslation(dieNo);
			reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalBlock = blockNo;
			reqPoolPtr->reqPool[reqSlotTag].nandInfo.physicalPage = 0;

			SelectLowLevelReqQ(reqSlotTag);
		}


	SyncAllLowLevelReqDone();
	xil_printf("Done.\r\n");
}


void EraseUserBlockSpace()
{
	unsigned int blockNo, dieNo, reqSlotTag;

	xil_printf("Erase User block space...wait for a minute...\r\n");

	for(blockNo=0 ; blockNo<USER_BLOCKS_PER_DIE ; blockNo++)
		for(dieNo=0 ; dieNo<USER_DIES ; dieNo++)
			if(!virtualBlockMapPtr->block[dieNo][blockNo].bad)
			{
				reqSlotTag = GetFromFreeReqQ();

				reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
				reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_ERASE;
				reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
				reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
				reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_NONE;
				reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;

				reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = Vorg2VsaTranslation(dieNo, blockNo, 0);

				SelectLowLevelReqQ(reqSlotTag);
			}

	SyncAllLowLevelReqDone();
	xil_printf("Done.\r\n");
}


void InitBlockDieMap()
{
	unsigned int dieNo;
	unsigned char eraseFlag = 1;

	xil_printf("Press 'X' to re-make the bad block table.\r\n");
	if (inbyte() == 'X')
	{
		EraseTotalBlockSpace();
		eraseFlag = 0;
	}

	InitDieMap();

	//make bad block table
	RecoverBadBlockTable(RESERVED_DATA_BUFFER_BASE_ADDR);

	//to prevent accessing bbtBlock by host
	for(dieNo=0 ; dieNo<USER_DIES ; dieNo++)
		phyBlockMapPtr->phyBlock[dieNo][bbtInfoMapPtr->bbtInfo[dieNo].phyBlock].bad = 1;

	RemapBadBlock();

	InitBlockMap();

	if(eraseFlag)
		EraseUserBlockSpace();

	InitCurrentBlockOfDieMap();
}

unsigned int AddrTransRead(unsigned int logicalSliceAddr)
{
	unsigned int virtualSliceAddr;

	if(logicalSliceAddr < SLICES_PER_SSD)
	{
		virtualSliceAddr = logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr;

		if(virtualSliceAddr != VSA_NONE)
			return virtualSliceAddr;
		else
			return VSA_FAIL;
	}
	else
		assert(!"[WARNING] Logical address is larger than maximum logical address served by SSD [WARNING]");
}

unsigned int AddrTransWrite(unsigned int logicalSliceAddr)
{
	unsigned int virtualSliceAddr;

	if(logicalSliceAddr < SLICES_PER_SSD)
	{
		InvalidateOldVsa(logicalSliceAddr);

		virtualSliceAddr = FindFreeVirtualSlice();

		logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr = virtualSliceAddr;
		virtualSliceMapPtr->virtualSlice[virtualSliceAddr].logicalSliceAddr = logicalSliceAddr;

		// Count this as a host write (logical page write)
		g_stat_host_write_count++;

		return virtualSliceAddr;
	}
	else
		assert(!"[WARNING] Logical address is larger than maximum logical address served by SSD [WARNING]");
}


unsigned int FindFreeVirtualSlice()
{
	unsigned int currentBlock, virtualSliceAddr, dieNo;

	dieNo = sliceAllocationTargetDie;
	currentBlock = virtualDieMapPtr->die[dieNo].currentBlock;

	if(virtualBlockMapPtr->block[dieNo][currentBlock].currentPage == USER_PAGES_PER_BLOCK)
	{
		currentBlock = GetFromFbList(dieNo, GET_FREE_BLOCK_NORMAL);

		if(currentBlock != BLOCK_FAIL)
			virtualDieMapPtr->die[dieNo].currentBlock = currentBlock;
		else
		{
			GarbageCollection(dieNo);
			currentBlock = virtualDieMapPtr->die[dieNo].currentBlock;

			if(virtualBlockMapPtr->block[dieNo][currentBlock].currentPage == USER_PAGES_PER_BLOCK)
			{
				currentBlock = GetFromFbList(dieNo, GET_FREE_BLOCK_NORMAL);
				if(currentBlock != BLOCK_FAIL)
					virtualDieMapPtr->die[dieNo].currentBlock = currentBlock;
				else
					assert(!"[WARNING] There is no available block [WARNING]");
			}
			else if(virtualBlockMapPtr->block[dieNo][currentBlock].currentPage > USER_PAGES_PER_BLOCK)
				assert(!"[WARNING] Current page management fail [WARNING]");
		}
	}
	else if(virtualBlockMapPtr->block[dieNo][currentBlock].currentPage > USER_PAGES_PER_BLOCK)
		assert(!"[WARNING] Current page management fail [WARNING]");


	virtualSliceAddr = Vorg2VsaTranslation(dieNo, currentBlock, virtualBlockMapPtr->block[dieNo][currentBlock].currentPage);
	virtualBlockMapPtr->block[dieNo][currentBlock].currentPage++;
	g_last_update_tick[dieNo][currentBlock] = ++g_cb_tick;
	ts_total_writes++;
	
	// Update statistics counters
	g_stat_nand_program_count++;      // Count NAND program (called by host write)
	g_block_write_cnt[dieNo][currentBlock]++;
	
	sliceAllocationTargetDie = FindDieForFreeSliceAllocation();
	dieNo = sliceAllocationTargetDie;
	return virtualSliceAddr;
}


unsigned int FindFreeVirtualSliceForGc(unsigned int copyTargetDieNo, unsigned int victimBlockNo)
{
	unsigned int currentBlock, virtualSliceAddr, dieNo;

	dieNo = copyTargetDieNo;
	if(victimBlockNo == virtualDieMapPtr->die[dieNo].currentBlock)
	{
		virtualDieMapPtr->die[dieNo].currentBlock = GetFromFbList(dieNo, GET_FREE_BLOCK_GC);
		if(virtualDieMapPtr->die[dieNo].currentBlock == BLOCK_FAIL)
			assert(!"[WARNING] There is no available block [WARNING]");
	}
	currentBlock = virtualDieMapPtr->die[dieNo].currentBlock;

	if(virtualBlockMapPtr->block[dieNo][currentBlock].currentPage == USER_PAGES_PER_BLOCK)
	{

		currentBlock = GetFromFbList(dieNo, GET_FREE_BLOCK_GC);

		if(currentBlock != BLOCK_FAIL)
			virtualDieMapPtr->die[dieNo].currentBlock = currentBlock;
		else
			assert(!"[WARNING] There is no available block [WARNING]");
	}
	else if(virtualBlockMapPtr->block[dieNo][currentBlock].currentPage > USER_PAGES_PER_BLOCK)
		assert(!"[WARNING] Current page management fail [WARNING]");


	virtualSliceAddr = Vorg2VsaTranslation(dieNo, currentBlock, virtualBlockMapPtr->block[dieNo][currentBlock].currentPage);
	virtualBlockMapPtr->block[dieNo][currentBlock].currentPage++;
	g_last_update_tick[dieNo][currentBlock] = ++g_cb_tick;
	
	// Update statistics (GC write - no host write increment)
	g_stat_nand_program_count++;      // Count NAND program for GC
	g_block_write_cnt[dieNo][currentBlock]++;
	
	return virtualSliceAddr;
}


unsigned int FindDieForFreeSliceAllocation()
{
	static unsigned char targetCh = 0;
	static unsigned char targetWay = 0;
	unsigned int targetDie;

	targetDie = Pcw2VdieTranslation(targetCh, targetWay);

	if(targetCh != (USER_CHANNELS - 1))
		targetCh = targetCh + 1;
	else
	{
		targetCh = 0;
		targetWay = (targetWay + 1) % USER_WAYS;
	}

	return targetDie;
}

void InvalidateOldVsa(unsigned int logicalSliceAddr)
{
	unsigned int virtualSliceAddr, dieNo, blockNo;

	virtualSliceAddr = logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr;

	if(virtualSliceAddr != VSA_NONE)
	{
		if(virtualSliceMapPtr->virtualSlice[virtualSliceAddr].logicalSliceAddr != logicalSliceAddr)
			return;

		dieNo = Vsa2VdieTranslation(virtualSliceAddr);
		blockNo = Vsa2VblockTranslation(virtualSliceAddr);

		// unlink
		SelectiveGetFromGcVictimList(dieNo, blockNo);
		virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt++;
		g_last_update_tick[dieNo][blockNo] = ++g_cb_tick;
		logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr = VSA_NONE;

		PutToGcVictimList(dieNo, blockNo, virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt);
		ts_total_invalidates++;
	}

}


void EraseBlock(unsigned int dieNo, unsigned int blockNo)
{
	unsigned int pageNo, virtualSliceAddr, reqSlotTag;

	reqSlotTag = GetFromFreeReqQ();

	reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
	reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_ERASE;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_NONE;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
	reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
	reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = Vorg2VsaTranslation(dieNo, blockNo, 0);
	reqPoolPtr->reqPool[reqSlotTag].nandInfo.programmedPageCnt = virtualBlockMapPtr->block[dieNo][blockNo].currentPage;

	SelectLowLevelReqQ(reqSlotTag);

	// block map indicated blockNo initialization
	virtualBlockMapPtr->block[dieNo][blockNo].free = 1;
	virtualBlockMapPtr->block[dieNo][blockNo].eraseCnt++;
	ts_total_erases++;
	virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt = 0;
	virtualBlockMapPtr->block[dieNo][blockNo].currentPage = 0;

	/* age update for cost-benefit */
	g_last_update_tick[dieNo][blockNo] = ++g_cb_tick;

	PutToFbList(dieNo, blockNo);

	for(pageNo=0; pageNo<USER_PAGES_PER_BLOCK; pageNo++)
	{
		virtualSliceAddr = Vorg2VsaTranslation(dieNo, blockNo, pageNo);
		virtualSliceMapPtr->virtualSlice[virtualSliceAddr].logicalSliceAddr = LSA_NONE;
	}
}

void PutToFbList(unsigned int dieNo, unsigned int blockNo) //fb means free block
{
	if(virtualDieMapPtr->die[dieNo].tailFreeBlock != BLOCK_NONE)
	{
		virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = virtualDieMapPtr->die[dieNo].tailFreeBlock;
		virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
		virtualBlockMapPtr->block[dieNo][virtualDieMapPtr->die[dieNo].tailFreeBlock].nextBlock = blockNo;
		virtualDieMapPtr->die[dieNo].tailFreeBlock = blockNo;
	}
	else
	{
		virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = BLOCK_NONE;
		virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
		virtualDieMapPtr->die[dieNo].headFreeBlock = blockNo;
		virtualDieMapPtr->die[dieNo].tailFreeBlock = blockNo;
	}

	virtualDieMapPtr->die[dieNo].freeBlockCnt++;
}

unsigned int GetFromFbList(unsigned int dieNo, unsigned int getFreeBlockOption) //fb means free block
{
	unsigned int evictedBlockNo;

	evictedBlockNo = virtualDieMapPtr->die[dieNo].headFreeBlock;

	if(getFreeBlockOption == GET_FREE_BLOCK_NORMAL)
	{
		if(virtualDieMapPtr->die[dieNo].freeBlockCnt <= RESERVED_FREE_BLOCK_COUNT)
			return BLOCK_FAIL;
	}
	else if(getFreeBlockOption == GET_FREE_BLOCK_GC)
	{
		if(evictedBlockNo == BLOCK_NONE)
			return BLOCK_FAIL;
	}
	else
		assert(!"[WARNING] Wrong getFreeBlockOption [WARNING]");

	if(virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock != BLOCK_NONE)
	{
		virtualDieMapPtr->die[dieNo].headFreeBlock = virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock;
		virtualBlockMapPtr->block[dieNo][virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock].prevBlock = BLOCK_NONE;
	}
	else
	{
		virtualDieMapPtr->die[dieNo].headFreeBlock = BLOCK_NONE;
		virtualDieMapPtr->die[dieNo].tailFreeBlock = BLOCK_NONE;
	}

	virtualBlockMapPtr->block[dieNo][evictedBlockNo].free = 0;
	virtualDieMapPtr->die[dieNo].freeBlockCnt--;

	virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock = BLOCK_NONE;
	virtualBlockMapPtr->block[dieNo][evictedBlockNo].prevBlock = BLOCK_NONE;

	return evictedBlockNo;
}


void UpdatePhyBlockMapForGrownBadBlock(unsigned int dieNo, unsigned int phyBlockNo)
{
	phyBlockMapPtr->phyBlock[dieNo][phyBlockNo].bad = BLOCK_STATE_BAD;

	bbtInfoMapPtr->bbtInfo[dieNo].grownBadUpdate = BBT_INFO_GROWN_BAD_UPDATE_BOOKED;
}


void UpdateBadBlockTableForGrownBadBlock(unsigned int tempBufAddr)
{
	unsigned int dieNo, phyBlockNo, tempBbtBufBaseAddr, tempBbtBufEntrySize;
	unsigned int tempBbtBufAddr[USER_DIES];
	unsigned char dieState[USER_DIES];
	unsigned char* bbtUpdater;

	//data buffer allocation
	tempBbtBufBaseAddr = tempBufAddr;
	tempBbtBufEntrySize = BYTES_PER_DATA_REGION_OF_PAGE + BYTES_PER_SPARE_REGION_OF_PAGE;
	for(dieNo = 0; dieNo < USER_DIES; dieNo++)
		tempBbtBufAddr[dieNo] = tempBbtBufBaseAddr + dieNo * USED_PAGES_FOR_BAD_BLOCK_TABLE_PER_DIE * tempBbtBufEntrySize;

	//create new bad block table
	for(dieNo = 0; dieNo < USER_DIES; dieNo++)
	{
		if(bbtInfoMapPtr->bbtInfo[dieNo].grownBadUpdate == BBT_INFO_GROWN_BAD_UPDATE_BOOKED)
		{
			for(phyBlockNo = 0; phyBlockNo < TOTAL_BLOCKS_PER_DIE; phyBlockNo++)
			{
				bbtUpdater = (unsigned char*)(tempBbtBufAddr[dieNo] + phyBlockNo);

				if(phyBlockNo != bbtInfoMapPtr->bbtInfo[dieNo].phyBlock)
					*bbtUpdater = phyBlockMapPtr->phyBlock[dieNo][phyBlockNo].bad;
				else
					*bbtUpdater = BLOCK_STATE_NORMAL;
			}

			dieState[dieNo] = DIE_STATE_BAD_BLOCK_TABLE_UPDATE;
		}
		else
			dieState[dieNo] = DIE_STATE_BAD_BLOCK_TABLE_HOLD;
	}

	//update bad block tables in flash
	SaveBadBlockTable(dieState, tempBbtBufAddr, tempBbtBufEntrySize);
}

