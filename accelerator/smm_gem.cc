//
// Created by alireza on 3/3/22.
//

#include "iostream"
#include "smm_gem.h"
#include <cmath>
#include <omp.h>

#include <iomanip>
#include "../transformer_layers/debuggerFunctions.h"
#include <thread>

#ifdef SIMD
#include <arm_neon.h>
#endif

#define W_DATA 4
#define KERNEL_DIM SA_SIZE
#define MAX_COL (SA_SIZE/W_DATA)
#define mem2d(data, data_len, row, col)   data[((row)*(data_len))+(col)]


void add8in32(uint32_t &memory, uint32_t &systolicResult);

//#define DEVELOP

#ifndef DEVELOP

/* CM Core Process (MVM)
* Instruction format: |____Opcode___|__rm__|_X|__ra__|__rn__|__rd__|
* Bits:               |31_________21|20__16|15|14__10|9____5|4____0|
* Binary layout:      |0000_0001_000|0_1000|_0|001_11|01_001|0_1010|
* Hex layout:         |__0____1____0|____8_|__|_1____|D____2|____A_|
* gem5 variables:     |_____________|_Op264|__|_Op364|_Op164|Dest64|
*
* Queueing arguments:
* -- rd = Systolic Array Output.
* -- rm = Unused.
* -- ra = Thread index.
* -- rn = Parameter value.
*/

uint64_t smmStream(uint64_t rn, uint64_t tid=0) {
    uint64_t res;

    __asm__ volatile(
    "MOV X7, %[input_k];"
    "MOV X8, %[input_i];"
    ".long 0x01081D2A;"
    "MOV %[output], X10;"
    : [output] "=r"(res)
    : [input_k] "r"(rn), [input_i] "r"(tid)
    : "x7", "x8", "x10"
    );

    return res;
}

/* CM Core Queue (MVM)
* Instruction format: |____Opcode___|__rm__|_X|__ra__|__rn__|__rd__|
* Bits:               |31_________21|20__16|15|14__10|9____5|4____0|
* Binary layout:      |0010_0001_000|0_1000|_0|001_11|01_001|0_1010|
* Hex layout:         |__2____1____0|____8_|__|_1____|D____2|____A_|
* gem5 variables:     |_____________|_Op264|__|_Op364|_Op164|Dest64|
*
* Queueing arguments:
* -- rd = Systolic Array output.
* -- rm = Parameter index.
* -- ra = Thread index.
* -- rn = Parameter value.
*/
uint64_t smmQueue(uint64_t rm, uint64_t rn, uint64_t tid=0) {
    uint64_t res;

    __asm__ volatile(
    "MOV X9, %[input_j];"
    "MOV X7, %[input_k];"
    "MOV X8, %[input_i];"
    ".long 0x21089D2A;"
    "MOV %[output], X10;"
    : [output] "=r"(res)
    : [input_j] "r"(rm), [input_i] "r"(tid), [input_k] "r"(rn)
    : "x7", "x8", "x9", "x10"
    );

    return res;
}

/* CM Core Parameter Write
 * Instruction format: |____Opcode___|__rm__|_?|__ra__|__rn__|__rd__|
 * Bits:               |31_________21|20__16|15|14__10|9____5|4____0|
 * Binary layout:      |0100_0001_000|0_1000|_0|001_11|01_001|0_1010|
 * Hex layout:         |__4____1____0|____8_|__|_1____|D____2|____A_|
 * gem5 variables:     |_____________|_Op264|__|_Op364|_Op164|Dest64|
 *
 * Queueing arguments:
* -- rd = Zero tile code.
* -- rm = Parameter index.
* -- ra = Thread index.
* -- rn = Parameter value.
 */
uint64_t smmParamWrite(uint64_t rm, uint64_t rn, int tid=0) {
    uint64_t res;

    __asm__ volatile(
    "MOV X9, %[input_j];"
    "MOV X7, %[input_k];"
    "MOV X8, %[input_i];"
    ".long 0x41081D2A;"
    "MOV %[output], X10;"
    : [output] "=r"(res)
    : [input_j] "r"(rm), [input_i] "r"(tid), [input_k] "r"(rn)
    : "x7", "x8", "x9", "x10"
    );

    return res;

}

#else

#include "systolic_m2m.hh"

SystolicMatrixMultiplication smm0 = SystolicMatrixMultiplication();
SystolicMatrixMultiplication smm1 = SystolicMatrixMultiplication();
SystolicMatrixMultiplication smm2 = SystolicMatrixMultiplication();
SystolicMatrixMultiplication smm3 = SystolicMatrixMultiplication();
SystolicMatrixMultiplication smmList[] = {smm0, smm1, smm2, smm3};

bool smmParamWrite(int rm, uint32_t ra, int tid) {
    return smmList[tid].loadWeights(rm, ra);
}

uint32_t smmQueue(int rm, uint32_t ra, int tid) {
    return smmList[tid].inputQueue(rm, ra);
}

uint32_t smmStream(uint32_t rn, int tid) {
    return smmList[tid].streamInOut(rn);
}

#endif


void smmComputeRWMA(std::size_t seq_len, const uint32_t *input, uint32_t *output, uint32_t *weights,
                    std::size_t input_size_, std::size_t output_size_) {

    int ROWS_IN_BLOCK = std::min(128, (int) (seq_len));
    int rowMaxL1 = std::min(64, (int) (input_size_)) / KERNEL_DIM;
    int ratio = 64 / std::min(64, (int) (input_size_));
    int colMaxL1 = std::min(32 * ratio, (int) (output_size_)) / KERNEL_DIM;

    int ROWS_IN_L2 = std::min(512 / ROWS_IN_BLOCK, (int) ceil((float) (seq_len) / (float) ROWS_IN_BLOCK));
    int rowMaxL2 = std::min(256, (int) (input_size_)) / KERNEL_DIM / rowMaxL1;
    int colMaxL2 = std::min(256, (int) (output_size_)) / KERNEL_DIM / colMaxL1;

    omp_set_num_threads(CORE_NUM); // set number of threads in "parallel" blocks

    int col_in_th = ROWS_IN_L2 / CORE_NUM;

#pragma omp parallel
    {
        int omp_id = omp_get_thread_num();
    for (int l2In = 0; l2In < (int) ceil((float) seq_len / (float) ROWS_IN_BLOCK / (float) ROWS_IN_L2); l2In++) {
        for (int l2Row = 0; l2Row < (input_size_ / KERNEL_DIM) / rowMaxL2 / rowMaxL1; l2Row++) {
            for (int l2Col = 0; l2Col < (output_size_ / KERNEL_DIM) / colMaxL2 / colMaxL1; l2Col++) {
                for (int tileInL2 = omp_id*col_in_th; tileInL2 < (omp_id + 1) * col_in_th; tileInL2++) {
                    for (int tileRowL2 = 0; tileRowL2 < rowMaxL2; tileRowL2++) {
                        for (int tileColL2 = 0; tileColL2 < colMaxL2; tileColL2++) {
                            for (int tileRowL1 = 0; tileRowL1 < rowMaxL1; tileRowL1++) {
                                for (int tileColL1 = 0; tileColL1 < colMaxL1; tileColL1++) {
                                    int tileRow = tileRowL2 * rowMaxL1 + tileRowL1;
                                    int tileCol = tileColL2 * colMaxL1 + tileColL1;
                                    int seqBlockIdx = l2In * ROWS_IN_L2 + tileInL2;
                                    // Load the kernel with the corresponding weight
                                    int rowStart = (l2Row * rowMaxL2 * rowMaxL1 + tileRow) * KERNEL_DIM;
                                    int colStart = (l2Col * colMaxL2 * colMaxL1 + tileCol) * KERNEL_DIM / W_DATA;
                                    int rowBlockSize = KERNEL_DIM;
                                    int colBlockSize = KERNEL_DIM / W_DATA;
                                    uint32_t *wPtr = weights + rowStart * (output_size_ / W_DATA);
                                    for (int i = rowStart; i < rowStart + rowBlockSize; i++) {
                                        for (int j = colStart; j < colStart + colBlockSize; j++) {
                                            uint32_t weight = *(wPtr + j);
                                            int weight_idx = (i - rowStart) * KERNEL_DIM + (j - colStart) * W_DATA;
                                            smmParamWrite(weight_idx, weight, omp_id);
                                        }
                                        wPtr += output_size_ / W_DATA;
                                    }

                                    // Process the multiplication
                                    int base_col_idx = (l2Row * rowMaxL2 * rowMaxL1 + tileRow) * MAX_COL;
                                    int seqBlockLen = std::min(ROWS_IN_BLOCK,
                                                               (int) (seq_len - seqBlockIdx * ROWS_IN_BLOCK));
                                    int outputIndex = 0;
                                    uint32_t *outPtr = output + seqBlockIdx * ROWS_IN_BLOCK * (output_size_ / W_DATA);
                                    uint32_t mult;
                                    const uint32_t *inPtr =
                                            input + base_col_idx + seqBlockIdx * ROWS_IN_BLOCK * (input_size_ / W_DATA);
                                    for (int i = 0; i < seqBlockLen; i++) {
                                        for (int j = 0; j < MAX_COL; j++) {
                                            if (j == MAX_COL - 1) {
                                                mult = smmStream(*(inPtr + j), omp_id);
                                            } else {
                                                mult = smmQueue(j % MAX_COL, *(inPtr + j), omp_id);
                                            }

                                            if ((i * MAX_COL + j) >=
                                                (MAX_COL * (2 * KERNEL_DIM - 1) -
                                                 1)) {    // check if the output is valid
                                                add8in32(
                                                        mem2d(outPtr, output_size_ / W_DATA, outputIndex / colBlockSize,
                                                              colStart + outputIndex % colBlockSize), mult);
                                                outputIndex++;
                                            }
                                        }
                                        inPtr += (input_size_ / W_DATA);
                                    }
                                    for (int i = seqBlockLen * MAX_COL;
                                         i < MAX_COL * (seqBlockLen + 2 * KERNEL_DIM - 1) - 1; i++) {
                                        if ((i % MAX_COL) == MAX_COL - 1) {
                                            mult = smmStream(0, omp_id);
                                        } else {
                                            mult = smmQueue(i % MAX_COL, 0, omp_id);
                                        }
                                        if (i >= (MAX_COL * (2 * KERNEL_DIM - 1) - 1)) { // check if the output is valid
                                            add8in32(mem2d(outPtr, output_size_ / W_DATA, outputIndex / colBlockSize,
                                                           colStart + outputIndex % colBlockSize), mult);
                                            outputIndex++;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    }
}

void smmComputeBWMA(std::size_t seq_len, uint32_t *input, uint32_t *output, uint32_t *weights,
                    std::size_t input_size_, std::size_t output_size_) {
    omp_set_num_threads(CORE_NUM); // set number of threads in "parallel" blocks
uint32_t *inPtr;
uint32_t *outPtr;
uint32_t* weightPtr;
int rowBlockSize = KERNEL_DIM;
int colBlockSize = KERNEL_DIM / W_DATA;
int col_in_th = output_size_ / KERNEL_DIM /  CORE_NUM;
# pragma omp parallel private(inPtr, outPtr, weightPtr)
{
    int id = omp_get_thread_num();
    int start_index = col_in_th * id;
    int end_index = start_index + col_in_th;
    weightPtr = weights + start_index * (input_size_ / KERNEL_DIM) * rowBlockSize * colBlockSize;
    for (int l2Col = start_index; l2Col < end_index; l2Col++) {
        for (int l2Row = 0; l2Row < input_size_ / KERNEL_DIM; l2Row++) {
            // Load the kernel with the corresponding weight

            for (int i = 0; i < rowBlockSize * colBlockSize; i++) {
                uint32_t weight = *(weightPtr++);
                smmParamWrite(i * W_DATA, weight, id);
            }
            // Process the multiplication
            int base_col_idx = l2Row * MAX_COL * seq_len;
            outPtr = output + l2Col * MAX_COL * seq_len;
            uint32_t mult;
            inPtr = input + base_col_idx;
            for (int i = 0; i < seq_len; i++) {
                for (int j = 0; j < MAX_COL; j++) {
                    uint32_t content = *(inPtr);
                    if (j == MAX_COL - 1) {
                        mult = smmStream(*(inPtr++), id);
                    } else {
                        mult = smmQueue(j % MAX_COL, *(inPtr++), id);
                    }

                    if ((i * MAX_COL + j) >= (MAX_COL * (2 * KERNEL_DIM - 1) - 1)) {
                        // check if the output is valid
                        add8in32(*(outPtr++), mult);
                    }
                }
            }
            for (int i = seq_len * MAX_COL;
                 i < MAX_COL * (seq_len + 2 * KERNEL_DIM - 1) - 1; i++) {
                if ((i % MAX_COL) == MAX_COL - 1) {
                    mult = smmStream(0, id);
                } else {
                    mult = smmQueue(i % MAX_COL, 0, id);
                }
                if (i >= (MAX_COL * (2 * KERNEL_DIM - 1) - 1)) { // check if the output is valid
                    add8in32(*(outPtr++), mult);
                }
            }
        }
    }
}
}

void add8in32(uint32_t &memory, uint32_t &systolicResult) {
    /*
     * This function separates every 32-bit input to four 8-bit integers and add them. Then, packing again and
     * make a 32-bit  unsigned int.
     */
    auto *mem_ptr = (int8_t *) (&memory);
    auto *sys_ptr = (int8_t *) (&systolicResult);
    for (int i = 0; i < W_DATA; i++) {
        *mem_ptr = (int8_t) (*mem_ptr + *sys_ptr);
        mem_ptr++;
        sys_ptr++;
    }
}

void conventionalCompute(std::size_t seq_len, const uint32_t *input, uint32_t *output, uint32_t *weight,
                         std::size_t input_size_, std::size_t output_size_) {
    for (int length = 0; length < seq_len; length++) {
        for (int out_idx = 0; out_idx < (output_size_ / W_DATA); out_idx++) {
            // std::cout<< "out : " << out_idx << std::endl;
            auto *weight_ptr = (int8_t *) (weight + out_idx);
            auto *output_ptr = (int8_t *) (output + (length * output_size_ / W_DATA) + out_idx);
            for (int w = 0; w < W_DATA; w++) {
                auto *input_ptr = (int8_t *) (input + (length * input_size_ / W_DATA));
                int sum = 0;
                for (int i = 0; i < input_size_; i++) {
                    sum += *(weight_ptr + (i + 3 - 2 * (i % W_DATA)) * output_size_ + w) * (*(input_ptr));
                    input_ptr++;
                }
                *(output_ptr + w) = (int8_t) sum;
            }
        }
    }
}

void tiledCompute(std::size_t seq_len, const uint32_t *input, uint32_t *output, uint32_t *weight,
                  std::size_t input_size_, std::size_t output_size_) {
    int ROWS_IN_BLOCK = std::min(128, (int) (seq_len));
    int COLS_IN_BLOCK = std::min(32, (int) (input_size_));
    int ratio = 32 / COLS_IN_BLOCK;
    int W_COL_BLOCKS = std::min(32 * ratio, (int) (output_size_));

    int ROWS_IN_L2 = std::min(512 / ROWS_IN_BLOCK, (int) (seq_len / ROWS_IN_BLOCK));
    int COLS_IN_L2 = std::min(256 / COLS_IN_BLOCK, (int) (input_size_ / COLS_IN_BLOCK));
    int W_COL_IN_L2 = std::min(256 / W_COL_BLOCKS, (int) (output_size_ / W_COL_BLOCKS));

    for (int blk_row_idx = 0; blk_row_idx < (seq_len / ROWS_IN_BLOCK / ROWS_IN_L2); blk_row_idx++) {
        for (int blk_col_idx = 0; blk_col_idx < (input_size_ / COLS_IN_BLOCK / COLS_IN_L2); blk_col_idx++) {
            for (int w_blk_col_idx = 0; w_blk_col_idx < (output_size_ / W_COL_BLOCKS / W_COL_IN_L2); w_blk_col_idx++) {
                for (int l2_row_idx = 0; l2_row_idx < ROWS_IN_L2; l2_row_idx++) {
                    for (int l2_col_idx = 0; l2_col_idx < COLS_IN_L2; l2_col_idx++) {
                        for (int l2_w_idx = 0; l2_w_idx < W_COL_IN_L2; l2_w_idx++) {
                            for (int i = 0; i < ROWS_IN_BLOCK; i++) {
                                auto *input_ptr = (int8_t *) (input +
                                                              (((blk_row_idx * ROWS_IN_L2 + l2_row_idx) *
                                                                ROWS_IN_BLOCK + i) * input_size_ / W_DATA) +
                                                              // index of the input row
                                                              (blk_col_idx * COLS_IN_L2 + l2_col_idx) * COLS_IN_BLOCK /
                                                              W_DATA);   // block index
                                auto *output_ptr = (int8_t *) (output +
                                                               (((blk_row_idx * ROWS_IN_L2 + l2_row_idx) *
                                                                 ROWS_IN_BLOCK + i) * output_size_ / W_DATA) +
                                                               (w_blk_col_idx * W_COL_IN_L2 + l2_w_idx) * W_COL_BLOCKS /
                                                               W_DATA);
                                auto *weight_ptr = (int8_t *) (weight +
                                                               (blk_col_idx * COLS_IN_L2 + l2_col_idx) * COLS_IN_BLOCK *
                                                               output_size_ / W_DATA +
                                                               (w_blk_col_idx * W_COL_IN_L2 + l2_w_idx) * W_COL_BLOCKS /
                                                               W_DATA);
                                for (int j = 0; j < W_COL_BLOCKS; j++) {
                                    int sum = 0;
                                    for (int k = 0; k < COLS_IN_BLOCK; k++) {
                                        sum += *(input_ptr + k) *
                                               *(weight_ptr + (k + 3 - 2 * (k % W_DATA)) * output_size_ + j);
                                        // a bias is added because of the endianness
                                    }
                                    *(output_ptr + j) = (int8_t) ((*(output_ptr + j)) + sum);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

#ifdef SIMD

void print_int8 (int8x16_t data, char* name) {
    int i;
    static int8_t p[16];

    vst1q_s8 (p, data);

    printf ("%s = ", name);
    for (i = 0; i < 16; i++) {
        printf ("%02d ", p[i]);
    }
    printf ("\n");
}

bool is_all_zero_int8x16(int8x16_t vec) {
    // Create a zero vector for comparison
    int8x16_t zero_vec = vdupq_n_s8(0);

    // Compare the input vector with the zero vector element-wise
    uint8x16_t cmp_result = vceqq_s8(vec, zero_vec);

    // Combine the compare results into a single integer
    uint64x2_t cmp_result_pair = vreinterpretq_u64_u8(cmp_result);
    uint64_t combined_result = vgetq_lane_u64(cmp_result_pair, 0) & vgetq_lane_u64(cmp_result_pair, 1);

    // Check if all the elements are zero
    return combined_result == UINT64_MAX;
}


void simdComputeRWMA(size_t seq_len, const uint32_t * input, uint32_t * output, uint32_t * weight,
                 size_t input_size_, size_t output_size_) {

    int ROWS_IN_BLOCK = 16;
    int COLS_IN_BLOCK = 16;
    int W_COL_BLOCKS = 16;

    int ROWS_IN_L2 = (int) (seq_len / ROWS_IN_BLOCK);
    int COLS_IN_L2 = (int) (input_size_ / COLS_IN_BLOCK);
    int W_COL_IN_L2 = (int) (output_size_ / W_COL_BLOCKS);

    int8x16_t A[16];
    int8x16_t B[16];
    int8x16_t C[16];

    int counter = 0;
    int total_counter =0;

    for (int l2_w_idx = 0; l2_w_idx < COLS_IN_L2; l2_w_idx++) {
        for (int l2_col_idx = 0; l2_col_idx < W_COL_IN_L2; l2_col_idx++) {
            int B_idx = (l2_col_idx) * COLS_IN_BLOCK + (l2_w_idx) * W_COL_BLOCKS * output_size_;
            int8_t* weight8_t = (int8_t * ) weight;


            for (int i = 0; i < 16; ++i) {
                B[i] = vld1q_s8(weight8_t + B_idx + i*output_size_);
            }

            for (int l2_row_idx = 0; l2_row_idx < ROWS_IN_L2; l2_row_idx++) {

                for (int i=0; i<16; i++)
                    C[i]=vmovq_n_s8(0);


                int8_t* output8_t = (int8_t * ) output;
                int C_idx = ((l2_row_idx) * ROWS_IN_BLOCK) * output_size_ + (l2_col_idx) * COLS_IN_BLOCK ;

                //                bool print_bool = (l2_row_idx == 0 && l2_col_idx == 0 && l2_w_idx == 0);
                int A_idx = ((l2_row_idx * ROWS_IN_BLOCK) * input_size_) +  (l2_w_idx) * COLS_IN_BLOCK ;
                int8_t* input8_t = (int8_t * ) input;

                for (int i=0; i<16; i++)
                    A[i] = vld1q_s8(input8_t + A_idx + i* input_size_);

                for (int k=0; k< 16; k++){
                    for (int i=0; i<16; i++){
                        C[k] = vmlaq_s8(C[k], B[i], vmovq_n_s8(vgetq_lane_s8(A[k], 4*(i/4) +3-(i%4))));
                    }
                }

                for (int i = 0; i < 16; ++i) {
                    // Load current values from the output array
                    int8x16_t curr_C = vld1q_s8(output8_t + C_idx + i * output_size_);

                    // Add the new values to the current values
                    int8x16_t new_C = vaddq_s8(curr_C, C[i]);

                    // Store the updated values back into the output array
                    vst1q_s8(output8_t + C_idx + i * output_size_, new_C);
                }

            }

        }
    }
}



void simdComputeBWMA(size_t seq_len, const uint32_t * input, uint32_t * output, uint32_t * weight,
                    size_t input_size_, size_t output_size_) {

    int ROWS_IN_BLOCK = 16;
    int COLS_IN_BLOCK = 16;
    int W_COL_BLOCKS = 16;

    int ROWS_IN_L2 = (int) (seq_len / ROWS_IN_BLOCK);
    int COLS_IN_L2 = (int) (input_size_ / COLS_IN_BLOCK);
    int W_COL_IN_L2 = (int) (output_size_ / W_COL_BLOCKS);

    int8x16_t A[16];
    int8x16_t B[16];
    int8x16_t C[16];

    int8_t* weight8_t = (int8_t * ) weight;
    for (int l2_col_idx = 0; l2_col_idx < W_COL_IN_L2; l2_col_idx++) {
        for (int l2_w_idx = 0; l2_w_idx < COLS_IN_L2; l2_w_idx++) {

            for (int i = 0; i < 16; ++i) {
                B[i] = vld1q_s8(weight8_t);
                weight8_t += 16;
            }

            int A_idx = l2_w_idx * COLS_IN_BLOCK * (int) seq_len ;
            int8_t* input8_t = (int8_t * ) input + A_idx;

            int C_idx = l2_col_idx * COLS_IN_BLOCK * (int) seq_len ;
            int8_t* output8_t = (int8_t *) output + C_idx;


            for (int l2_row_idx = 0; l2_row_idx < ROWS_IN_L2; l2_row_idx++) {

                for (int i=0; i<16; i++)
                    C[i]=vmovq_n_s8(0);


                for (int i=0; i<16; i++){
                    A[i] = vld1q_s8(input8_t);
                    input8_t += 16;
                }

                for (int k=0; k< 16; k++){
                    for (int i=0; i<16; i++){
                        C[k] = vmlaq_s8(C[k], B[i], vmovq_n_s8(vgetq_lane_s8(A[k], 4*(i/4) +3-(i%4))));
                    }
                }

                for (int i = 0; i < 16; ++i) {
                    // Load current values from the output array
                    int8x16_t curr_C = vld1q_s8(output8_t);

                    // Add the new values to the current values
                    int8x16_t new_C = vaddq_s8(curr_C, C[i]);

                    // Store the updated values back into the output array
                    vst1q_s8(output8_t, new_C);

                    output8_t += 16;
                }

            }

        }
    }
}
#endif


