/*
 * Copyright (c) 2020 EPFL
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Alireza Amirshahi
 */

#ifndef __SYSTOLIC_M2M_H__
#define __SYSTOLIC_M2M_H__

#include "arch/arm/system.hh"
#include "dev/io_device.hh"
#include "debug/SMM.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "params/SystolicMatrixMultiplication.hh"

#include <vector>

#define KERNEL_DIM 16
#define W_DATA 4
#define MAX_COL 4

#define mem2d(data,data_len,row,col)   data[((row)*(data_len))+(col)]

class ArmSystem;
class BaseCPU;

struct SATile {
    SATile():
    weights(new int8_t[KERNEL_DIM * KERNEL_DIM]),
    inputMemory(new int8_t[KERNEL_DIM * KERNEL_DIM]),
    outputMemory(new int32_t[KERNEL_DIM * (KERNEL_DIM+1)]),
    inWaitingMemory(new int8_t[KERNEL_DIM * KERNEL_DIM]),
    outWaitingMemory(new uint8_t[KERNEL_DIM * KERNEL_DIM])
    {
        for (int i = 0; i < KERNEL_DIM*KERNEL_DIM; i++) {
            inputMemory[i] = 0;
            weights[i] = 0;
            inWaitingMemory[i] = 0;
            outWaitingMemory[i] = 0;
            outputMemory[i] = 0;
        }

        for (int i = 0; i < KERNEL_DIM; i++) {
            outputMemory[(KERNEL_DIM* KERNEL_DIM)+i] = 0;
        }
    }
    
    int8_t * weights;
    int8_t * inputMemory;
    int32_t * outputMemory;
    int8_t * inWaitingMemory;
    uint8_t * outWaitingMemory;
    bool non_zero_tile = false;
};

class SystolicMatrixMultiplication : public BasicPioDevice {
  private:
      
      std::vector<BaseCPU *> cpus;
      
      std::vector<SATile *> tiles;
      
    // System this ACM belongs to.
    ArmSystem * system;
    
    
    
  public:
      typedef SystolicMatrixMultiplicationParams Params;
      const Params * params() const {
          return dynamic_cast<const Params *>(_params);
      }
    SystolicMatrixMultiplication(const Params * p);
    ~SystolicMatrixMultiplication();
    void init() override;
    
    bool loadWeights(int tid, int idx, uint32_t  val);
    uint32_t inputQueue(int tid, int col, uint32_t  val);
    void printWeights();
    uint32_t readFlag(int tid, uint32_t val);
    uint32_t streamInOut(int tid, uint32_t val);
    

    // Required by SimObject.
    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;
    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;
    
    AddrRangeList getAddrRanges() const override;
    
    
};

#endif // __SYSTOLIC_M2M_H__
