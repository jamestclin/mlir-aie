// (c) Copyright 2019 Xilinx Inc. All Rights Reserved.

#include "mlir/Transforms/Passes.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Location.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Translation.h"
#include "mlir/Target/LLVMIR.h"

#include "llvm/IR/Module.h"
#include "llvm/Support/TargetSelect.h"

#include "AIEDialect.h"
#include "AIENetlistAnalysis.h"

using namespace mlir;
using namespace xilinx;
using namespace xilinx::AIE;

namespace xilinx {
namespace AIE {

std::string tileInstStr(StringRef col, StringRef row) {
  std::string str;
  llvm::raw_string_ostream rss(str);
  rss << "&(TileInst" << "[" << col << "][" << row << "])";
  return str;
}

std::string tileDMAInstStr(StringRef col, StringRef row) {
  std::string str;
  llvm::raw_string_ostream rss(str);
  rss << "&(TileDMAInst" << "[" << col << "][" << row << "])";
  return str;
}

// Output the buffer map for the given buffer operations, with the given offset.
// The offset is different depending on where the buffers are accessed from.
void writeBufferMap(raw_ostream &output, ArrayRef<BufferOp> buffers,
                    int offset, NetlistAnalysis &NL) {
  for (auto buf : buffers) {
    auto symbolAttr =
      buf.getOperation()->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName());
    StringRef bufName = std::string(symbolAttr.getValue());
    int bufferBaseAddr = NL.getBufferBaseAddress(buf);
    MemRefType t = buf.getType().cast<MemRefType>();
    int numBytes = t.getSizeInBits() / 8;
    output << "_symbol " <<
      bufName << " " <<
      "0x" << llvm::utohexstr(offset + bufferBaseAddr) << " " <<
      numBytes << '\n';
  }
}

void registerAIETranslations() {
  TranslateFromMLIRRegistration
    registrationLLVM("aie-generate-llvmir",
    [](ModuleOp module, raw_ostream &output) {
      llvm::LLVMContext llvmContext;
      auto llvmModule = mlir::translateModuleToLLVMIR(module, llvmContext);
      if (!llvmModule) {
        llvm::errs() << "Failed to emit LLVM IR\n";
        return failure();
      }

      output << *llvmModule;
      return success();
    }, 
    [](DialectRegistry &registry) {
      registry.insert<xilinx::AIE::AIEDialect>();
      registry.insert<StandardOpsDialect>();
      registry.insert<LLVM::LLVMDialect>();
    });

  TranslateFromMLIRRegistration
    registrationMMap("aie-generate-mmap", [](ModuleOp module, raw_ostream &output) {
      DenseMap<std::pair<int, int>, Operation *> tiles;
      DenseMap<Operation *, CoreOp> cores;
      DenseMap<Operation *, MemOp> mems;
      DenseMap<std::pair<Operation *, int>, LockOp> locks;
      DenseMap<Operation *, SmallVector<BufferOp, 4>> buffers;
      DenseMap<Operation *, SwitchboxOp> switchboxes;

      NetlistAnalysis NL(module, tiles, cores, mems, locks, buffers, switchboxes);
      NL.collectTiles(tiles);
      NL.collectBuffers(buffers);

      for (auto tile : tiles) {
        Operation *srcTileOp = tile.second;
        std::pair<int, int> srcCoord = NL.getCoord(srcTileOp);
        int srcCol = srcCoord.first;
        int srcRow = srcCoord.second;

        output << "// Tile(" << srcCol << ", " << srcRow << ")\n";
        output << "// Memory map: name base_address num_bytes\n";

        auto doBuffer = [&](Optional<TileID> tile, int offset) {
          if(tiles.count(tile.getValue()))
            writeBufferMap(output, buffers[tiles[tile.getValue()]], offset, NL);
        };
        if(auto tile = getMemSouth(srcCoord)) doBuffer(tile, 0x00020000);
        if(auto tile = getMemWest(srcCoord))  doBuffer(tile, 0x00028000);
        if(auto tile = getMemNorth(srcCoord)) doBuffer(tile, 0x00030000);
        if(auto tile = getMemEast(srcCoord))  doBuffer(tile, 0x00038000);
      }
      return success();
    }, 
    [](DialectRegistry &registry) {
      registry.insert<xilinx::AIE::AIEDialect>();
      registry.insert<StandardOpsDialect>();
      registry.insert<LLVM::LLVMDialect>();
    });

  TranslateFromMLIRRegistration
    registrationXAIE("aie-generate-xaie", [](ModuleOp module, raw_ostream &output) {
        StringRef enable  = "XAIE_ENABLE";
        StringRef disable = "XAIE_DISABLE";
        StringRef resetDisable = "XAIE_RESETDISABLE";

        DenseMap<std::pair<int, int>, Operation *> tiles;
        DenseMap<Operation *, CoreOp> cores;
        DenseMap<Operation *, MemOp> mems;
        DenseMap<std::pair<Operation *, int>, LockOp> locks;
        DenseMap<Operation *, SmallVector<BufferOp, 4>> buffers;
        DenseMap<Operation *, SwitchboxOp> switchboxes;

        NetlistAnalysis NL(module, tiles, cores, mems, locks, buffers, switchboxes);
        NL.collectTiles(tiles);
        NL.collectBuffers(buffers);

        output << "void mlir_initialize_cores() {\n";
        // Core configuration
        // Activate a core tile
        // void XAieTile_CoreControl(XAieGbl_Tile *TileInstPtr, u8 Enable, u8 Reset);
        for (auto tileOp : module.getOps<TileOp>()) {
          int col = tileOp.colIndex();
          int row = tileOp.rowIndex();
          output << "XAieTile_CoreControl("
                 << tileInstStr(std::to_string(col), std::to_string(row)) << ", "
                 << enable  << ", "
                 << disable <<
                 ");\n";
        }
        output << "} // mlir_initialize_cores\n\n";

        output << "void mlir_configure_dmas() {\n";

        // DMA configuration
        // XAieDma_TileSetStartBd(DmaInstPtr, ChNum, BdStart)
        // u32 XAieDma_TileSoftInitialize(XAieGbl_Tile *TileInstPtr, XAieDma_Tile *DmaInstPtr);
        // u32 XAieDma_TileInitialize(XAieGbl_Tile *TileInstPtr, XAieDma_Tile *DmaInstPtr);
        // void XAieDma_TileBdSetLock(XAieDma_Tile *DmaInstPtr, u8 BdNum, u8 AbType, u8 LockId, u8 LockRelEn, u8 LockRelVal, u8 LockAcqEn, u8 LockAcqVal);
        // void XAieDma_TileBdSetXy2d(XAieDma_Tile *DmaInstPtr, u8 BdNum, u8 XyType, u16 Incr, u16 Wrap, u16 Offset);
        // void XAieDma_TileBdSetIntlv(XAieDma_Tile *DmaInstPtr, u8 BdNum, u8 IntlvMode, u8 IntlvDb, u8 IntlvCnt, u16 IntlvCur);
        // void XAieDma_TileBdSetPkt(XAieDma_Tile *DmaInstPtr, u8 BdNum, u8 PktEn, u8 PktType, u8 PktId);
        // void XAieDma_TileBdSetAdrLenMod(XAieDma_Tile *DmaInstPtr, u8 BdNum, u16 BaseAddrA, u16 BaseAddrB, u16 Length, u8 AbMode, u8 FifoMode);
        // void XAieDma_TileBdSetNext(XAieDma_Tile *DmaInstPtr, u8 BdNum, u8 NextBd);
        // void XAieDma_TileBdWrite(XAieDma_Tile *DmaInstPtr, u8 BdNum);
        // void XAieDma_TileBdClear(XAieDma_Tile *DmaInstPtr, u8 BdNum);
        // void XAieDma_TileBdClearAll(XAieDma_Tile *DmaInstPtr);
        // u32 XAieDma_TileChControl(XAieDma_Tile *DmaInstPtr, u8 ChNum, u8 Reset, u8 Enable);
        // u32 XAieDma_TileChReset(XAieDma_Tile *DmaInstPtr, u8 ChNum);
        // u32 XAieDma_TileChResetAll(XAieDma_Tile *DmaInstPtr);
        for (auto memOp : module.getOps<MemOp>()) {
          int col = memOp.colIndex();
          int row = memOp.rowIndex();
          output << "XAieDma_TileInitialize(" <<
                    tileInstStr(std::to_string(col), std::to_string(row)) << ", " <<
                    tileDMAInstStr(std::to_string(col), std::to_string(row)) << ");\n";
          output << "XAieDma_TileBdClearAll(" <<
                    tileDMAInstStr(std::to_string(col), std::to_string(row)) << ");\n";
          output << "XAieDma_TileChResetAll(" <<
                    tileDMAInstStr(std::to_string(col), std::to_string(row)) << ");\n";

          DenseMap<Block *, int> blockMap;
          Block *endBlock = &memOp.body().back();
          // For each channel, which bdnum does it start with.
          std::vector<int> channelMap(4);

          {
            // Assign each block a BD number
            int bdNum = 0;
            bool foundBd = false;
            for (auto &block : memOp.body()) {
              for (auto op : block.getOps<DMABDOp>()) {
                foundBd = true;
              }
              if (foundBd) {
                blockMap[&block] = bdNum;
                bdNum++;
              }
            }
          }
          for (auto &block : memOp.body()) {
            bool foundBd = false;
            int len = 0;
            int offsetA = 0;
            int offsetB = 0;
            int BaseAddrA = 0;
            int BaseAddrB = 0;
            bool hasA = false;
            bool hasB = false;
            StringRef bufA = "0";
            StringRef bufB = "0";
            StringRef AbMode    = disable;
            StringRef FifoMode  = disable; // FIXME: when to enable FIFO mode?
            
            for (auto op : block.getOps<DMABDOp>()) {
              foundBd = true;
              len = op.getLenValue();
              if (op.isA()) {
                BaseAddrA = NL.getBufferBaseAddress(op.buffer().getDefiningOp());
                offsetA = op.getOffsetValue();
                bufA = "XAIEDMA_TILE_BD_ADDRA";
                hasA = true;
              }
              if (op.isB()) {
                BaseAddrB = NL.getBufferBaseAddress(op.buffer().getDefiningOp());
                offsetB = op.getOffsetValue();
                bufB = "XAIEDMA_TILE_BD_ADDRB";
                hasB = true;
              }
            }

            if (hasA && hasB) {
              AbMode = enable;
            }
            int acqValue = 0, relValue = 0;
            StringRef acqEnable = disable;
            StringRef relEnable = disable;
            int lockID;
            for (auto op : block.getOps<UseLockOp>()) {
              LockOp lock = dyn_cast<LockOp>(op.lock().getDefiningOp());
              lockID = lock.getLockID();
              if (op.acquire()) {
                acqEnable = enable;
                acqValue = op.getLockValue();
              } else if (op.release()) {
                relEnable = enable;
                relValue = op.getLockValue();
              }
            }

            int bdNum = blockMap[&block];
            if (foundBd) {
              if (hasA) {
                output << "XAieDma_TileBdSetLock(" <<
                          tileDMAInstStr(std::to_string(col), std::to_string(row)) << ", " <<
                          " /* bd */ "  << bdNum << ", " <<
                          bufA << ", " <<
                          " /* lockID */ " << lockID << ", " <<
                          relEnable << ", " <<
                          " /* release */ "  << relValue << ", " <<
                          acqEnable << ", " <<
                          " /* acquire */ "  << acqValue << ");\n";
              }
              if (hasB) {
                output << "XAieDma_TileBdSetLock(" <<
                          tileDMAInstStr(std::to_string(col), std::to_string(row)) << ", " <<
                          " /* bd */ "  << bdNum << ", " <<
                          bufB << ", " <<
                          " /* lockID */ " << lockID << ", " <<
                          relEnable << ", " <<
                          " /* release */ "  << relValue << ", " <<
                          acqEnable << ", " <<
                          " /* acquire */ "  << acqValue << ");\n";
              }

              output << "XAieDma_TileBdSetAdrLenMod(" <<
                        tileDMAInstStr(std::to_string(col), std::to_string(row)) << ", " <<
                        " /* bd */ "  << bdNum << ", " <<
                        " /* addrA */ "  << "0x" << llvm::utohexstr(BaseAddrA + offsetA) << ", " <<
                        " /* addrB */ "  << "0x" << llvm::utohexstr(BaseAddrB + offsetB) << ", " <<
                        " /* len */ "  << len << ", " <<
                        " /* ABMode */ "  << AbMode << ", " <<
                        " /* FIFOMode */ "  << FifoMode << ");\n";

              Block *nextBlock =
                  block.getSuccessors()[0]; // should have only one successor
                                            // block
              if (nextBlock != endBlock) {
                int nextBdNum = blockMap[nextBlock];
                output << "XAieDma_TileBdSetNext("
                       << tileDMAInstStr(std::to_string(col),
                                         std::to_string(row))
                       << ", "
                       << " /* bd */ " << bdNum << ", "
                       << " /* nextbd */ " << nextBdNum << ");\n";
              }
              output << "XAieDma_TileBdWrite("
                     << tileDMAInstStr(std::to_string(col), std::to_string(row))
                     << ", "
                     << " /* bd */ " << bdNum << ");\n";
            }

            for (auto op : block.getOps<CondBranchOp>()) {
              DMAStartOp dmaSt = dyn_cast<DMAStartOp>(op.getCondition().getDefiningOp());
              channelMap[(int)dmaSt.dmaChan()] = blockMap[op.getTrueDest()];
            }
          }

          for (auto &block : memOp.body()) {
            for (auto op : block.getOps<DMAStartOp>()) {
              int bdNum = channelMap[(int)op.dmaChan()];

              output << "XAieDma_TileSetStartBd("
                     << tileDMAInstStr(std::to_string(col), std::to_string(row))
                     << ", "
                     << "XAIEDMA_TILE_CHNUM_" << stringifyDMAChan(op.dmaChan())
                     << ", "
                     << " /* bd */ " << bdNum << ");\n";
              output << "XAieDma_TileChControl("
                     << tileDMAInstStr(std::to_string(col), std::to_string(row))
                     << ", "
                     << "XAIEDMA_TILE_CHNUM_" << stringifyDMAChan(op.dmaChan())
                     << ", " << resetDisable << ", " << enable << ");\n";
            }
          }
        }
        output << "} // mlir_configure_dmas\n\n";

        output << "void mlir_initialize_locks() {\n";
        // Lock configuration
        // u8 XAieTile_LockAcquire(XAieGbl_Tile *TileInstPtr, u8 LockId, u8 LockVal, u32 TimeOut);
        // u8 XAieTile_LockRelease(XAieGbl_Tile *TileInstPtr, u8 LockId, u8 LockVal, u32 TimeOut);
        for(auto op : module.getOps<UseLockOp>()) {
          int lockVal = op.getLockValue();
          int timeOut = op.getTimeout();
          LockOp lock = dyn_cast<LockOp>(op.lock().getDefiningOp());
          TileOp tile = dyn_cast<TileOp>(lock.tile().getDefiningOp());
          int col = tile.colIndex();
          int row = tile.rowIndex();
          int lockID = lock.getLockID();
          if (op.acquire()) {
            output << "XAieTile_LockAcquire(" <<
                      tileDMAInstStr(std::to_string(col), std::to_string(row)) << ", " <<
                      lockID << ", " <<
                      lockVal << ", " <<
                      timeOut << ");\n";
          } else if (op.release()) {
            output << "XAieTile_LockRelease(" <<
                      tileDMAInstStr(std::to_string(col), std::to_string(row)) << ", " <<
                      lockID << ", " <<
                      lockVal << ", " <<
                      timeOut << ");\n";
          }
        }
        output << "} // mlir_initialize_locks\n";

        output << "void mlir_configure_switchboxes() {\n";
        output << "  int x, y;\n";

        // StreamSwitch (switchbox) configuration
        // void XAieTile_StrmConnectCct(XAieGbl_Tile *TileInstPtr, u8 Slave, u8 Master, u8 SlvEnable);
        // void XAieTile_StrmConfigMstr(XAieGbl_Tile *TileInstPtr, u8 Master, u8 Enable, u8 PktEnable, u8 Config);
        // void XAieTile_StrmConfigSlv(XAieGbl_Tile *TileInstPtr, u8 Slave, u8 Enable, u8 PktEnable);
        // void XAieTile_StrmConfigSlvSlot(XAieGbl_Tile *TileInstPtr, u8 Slave, u8 Slot, u8 Enable, u32 RegVal);
        // void XAieTile_ShimStrmMuxConfig(XAieGbl_Tile *TileInstPtr, u32 Port, u32 Input);
        // void XAieTile_ShimStrmDemuxConfig(XAieGbl_Tile *TileInstPtr, u32 Port, u32 Output);
        // void XAieTile_StrmEventPortSelect(XAieGbl_Tile *TileInstPtr, u8 Port, u8 Master, u8 Id);

        // XAieTile_StrmConnectCct(&(TileInst[col+i][row]),
        //                         XAIETILE_STRSW_SPORT_TRACE((&(TileInst[col+i][row])), 1),
        //                         XAIETILE_STRSW_MPORT_NORTH((&(TileInst[col+i][row])), 0), XAIE_ENABLE);
        for(auto switchboxOp : module.getOps<SwitchboxOp>()) {
          Region &r = switchboxOp.connections();
          Block &b = r.front();
          bool isEmpty = b.getOps<ConnectOp>().empty() &&
            b.getOps<MasterSetOp>().empty() &&
            b.getOps<PacketRulesOp>().empty();
          bool isParam = false;

          if (isa<TileOp>(switchboxOp.tile().getDefiningOp())) {
            int col = switchboxOp.colIndex();
            int row = switchboxOp.rowIndex();
            if (!isEmpty) {
              output << "// Core Stream Switch column " << col << " row " << row << "\n";
              output << "x = " << col << ";\n";
              output << "y = " << row << ";\n";
            }
          } else if (AIE::SelectOp sel = dyn_cast<AIE::SelectOp>(switchboxOp.tile().getDefiningOp())) {
            // parameterize streamswitch's configuration
            isParam = true;
            HerdOp sourceHerd = dyn_cast<HerdOp>(sel.startHerd().getDefiningOp());
            auto symbolAttr = sourceHerd.getOperation()->getAttrOfType<StringAttr>(
                                SymbolTable::getSymbolAttrName());
            std::string sourceHerdName(symbolAttr.getValue());

            IterOp iterX     = dyn_cast<IterOp>(sel.iterX().getDefiningOp());
            IterOp iterY     = dyn_cast<IterOp>(sel.iterY().getDefiningOp());
            int startXValue  = iterX.getStartValue();
            int endXValue    = iterX.getEndValue();
            int strideXValue = iterX.getStrideValue();
            int startYValue  = iterY.getStartValue();
            int endYValue    = iterY.getEndValue();
            int strideYValue = iterY.getStrideValue();

            std::string startX(sourceHerdName + "_X + " + std::to_string(startXValue));
            std::string endX  (sourceHerdName + "_X + " + std::to_string(endXValue));
            std::string startY(sourceHerdName + "_Y + " + std::to_string(startYValue));
            std::string endY  (sourceHerdName + "_Y + " + std::to_string(endYValue));

            output << "for (x = " << startX << "; x < " << endX << "; x += " << strideXValue << ") {\n";
            output << "for (y = " << startY << "; y < " << endY << "; y += " << strideYValue << ") {\n";
          }

          for (auto connectOp : b.getOps<ConnectOp>()) {
            output << "XAieTile_StrmConnectCct(" <<
                      tileInstStr("x", "y") << ",\n";
            output << "\tXAIETILE_STRSW_SPORT_" <<
                      stringifyWireBundle(connectOp.sourceBundle()).upper() <<
                      "(" <<
                      tileInstStr("x", "y") << ", " <<
                      connectOp.sourceIndex() <<
                      "),\n";
            output << "\tXAIETILE_STRSW_MPORT_" <<
                      stringifyWireBundle(connectOp.destBundle()).upper() <<
                      "(" <<
                      tileInstStr("x", "y") << ", " <<
                      connectOp.destIndex() <<
                      "),\n";
            output << "\t" << enable << ");\n";
          }

          for (auto connectOp : b.getOps<MasterSetOp>()) {
            int mask = 0;
            int arbiter = -1;
            for (auto val : connectOp.amsels()) {
              AMSelOp amsel = dyn_cast<AMSelOp>(val.getDefiningOp());
              arbiter = amsel.arbiterIndex();
              int msel = amsel.getMselValue();
              mask |= (1 << msel);
            }

            output << "XAieTile_StrmConfigMstr(" <<
                      tileInstStr("x", "y") << ",\n";
            output << "\tXAIETILE_STRSW_MPORT_" <<
                      stringifyWireBundle(connectOp.destBundle()).upper() <<
                      "(" <<
                      tileInstStr("x", "y") << ", " <<
                      connectOp.destIndex() <<
                      "),\n";
            output << "\t" << enable << ",\n"; // port enable
            output << "\t" << enable << ",\n"; // packet enable
            output << "\tXAIETILE_STRSW_MPORT_CFGPKT(" <<
                      tileInstStr("x", "y") << ",\n";
            output << "\t\tXAIETILE_STRSW_MPORT_" <<
                      stringifyWireBundle(connectOp.destBundle()).upper() <<
                      "(" <<
                      tileInstStr("x", "y") << ", " <<
                      connectOp.destIndex() <<
                      "),\n";
            output << "\t\t" << disable << " /*drop_header*/,\n";
            output << "\t\t" << "0x" << llvm::utohexstr(mask) << " /*mask*/,\n"; // FIXME: compute mask for msel
            output << "\t\t" <<  arbiter << " /*arbiter*/));\n";
          }

          for (auto connectOp : b.getOps<PacketRulesOp>()) {
            int slot = 0;
            Block &block = connectOp.rules().front();
            for (auto slotOp : block.getOps<PacketRuleOp>()) {
              AMSelOp amselOp = dyn_cast<AMSelOp>(slotOp.amsel().getDefiningOp());
              int arbiter = amselOp.arbiterIndex();
              int msel    = amselOp.getMselValue();
              output << "XAieTile_StrmConfigSlvSlot(" <<
                        tileInstStr("x", "y") << ",\n";
              output << "\tXAIETILE_STRSW_SPORT_" <<
                        stringifyWireBundle(connectOp.sourceBundle()).upper() <<
                        "(" <<
                        tileInstStr("x", "y") << ", " <<
                        connectOp.sourceIndex() <<
                        "),\n";
              output << "\t" << slot << " /*slot*/,\n";
              output << "\t" << enable << ",\n";
              output << "\tXAIETILE_STRSW_SLVSLOT_CFG(" <<
                        tileInstStr("x", "y") << ",\n";
              output << "\t\tXAIETILE_STRSW_SPORT_" <<
                        stringifyWireBundle(connectOp.sourceBundle()).upper() <<
                        "(" <<
                        tileInstStr("x", "y") << ", " <<
                        connectOp.sourceIndex() <<
                        "),\n";
              output << "\t\t" << slot << " /*slot*/,\n";
              output << "\t\t" << "0x" << llvm::utohexstr(slotOp.valueInt()) << " /*ID value*/,\n";
              output << "\t\t" << "0x" << llvm::utohexstr(slotOp.maskInt()) << " /*mask*/,\n";
              output << "\t\t" << enable << ",\n";
              output << "\t\t" << msel << " /*msel*/,\n";
              output << "\t\t" << arbiter << " /*arbiter*/));\n";
              slot++;
            }
          }

          if (isParam) {
            output << "}\n";
            output << "}\n";
          }
        }
        for(auto switchboxOp : module.getOps<ShimSwitchboxOp>()) {
          Region &r = switchboxOp.connections();
          Block &b = r.front();
          bool isEmpty = b.getOps<ConnectOp>().empty();
          int col = switchboxOp.col();
          if(!isEmpty) {
            output << "// Shim Switch column " << col << "\n";
          }
          for (auto connectOp : b.getOps<ConnectOp>()) {
            output << "XAieTile_StrmConnectCct(" <<
                      tileInstStr(std::to_string(col), "0") << ",\n";
            output << "\tXAIETILE_STRSW_SPORT_" <<
                      stringifyWireBundle(connectOp.sourceBundle()).upper() <<
                      "(" <<
                      tileInstStr(std::to_string(col), "0") << ", " <<
                      connectOp.sourceIndex() <<
                      "),\n";
            output << "\tXAIETILE_STRSW_MPORT_" <<
                      stringifyWireBundle(connectOp.destBundle()).upper() <<
                      "(" <<
                      tileInstStr(std::to_string(col), "0") << ", " <<
                      connectOp.destIndex() <<
                      "),\n";
            output << "\t" << enable << ");\n";
          }
        }
        
        output << "} // mlir_configure_switchboxes\n\n";

        return success();
      }, 
      [](DialectRegistry &registry) {
        registry.insert<xilinx::AIE::AIEDialect>();
        registry.insert<StandardOpsDialect>();
        registry.insert<LLVM::LLVMDialect>();
      });
}
}
}
