//
// Created by alireza on 3/3/22.
//

#include <cstdint>
#include "iostream"
#include "smm_gem.h"

#define W_DATA 4
#define MAX_COL 4
#define KERNEL_DIM 16
#define mem2d(data,data_len,row,col)   data[((row)*(data_len))+(col)]

void add8in32(uint32_t &memory, uint32_t &systolicResult);

/* CM Core Process (MVM)
* Instruction format: |____Opcode___|__rm__|_X|__ra__|__rn__|__rd__|
* Bits:               |31_________21|20__16|15|14__10|9____5|4____0|
* Binary layout:      |0000_0001_000|0_1000|_0|001_11|01_001|0_1010|
* Hex layout:         |__0____1____0|____8_|__|_1____|D____2|____A_|
* gem5 variables:     |_____________|_Op264|__|_Op364|_Op164|Dest64|
*
* Queueing arguments:
* -- rd = Parameter value.
* -- rm = Parameter x index.
* -- ra = Unused.
* -- rn = Parameter y index.
*/

uint64_t smmStream(uint64_t rn)
{
    uint64_t res;

    __asm__ volatile(
            "MOV X7, %[input_k];"
            ".long 0x01081D2A;"
            "MOV %[output], X10;"
            : [output] "=r" (res)
            : [input_k] "r" (rn)
            : "x7", "x10"
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
* -- rd = Parameter value.
* -- rm = Parameter x index.
* -- ra = Unused.
* -- rn = Parameter y index.
*/
uint64_t smmQueue(uint64_t rm, uint64_t rn)
{
    uint64_t res;

    __asm__ volatile(
            "MOV X9, %[input_j];"
            "MOV X7, %[input_k];"
            ".long 0x21089D2A;"
            "MOV %[output], X10;"
            : [output] "=r" (res)
            : [input_j] "r" (rm), [input_k] "r" (rn)
            : "x7", "x9", "x10"
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
 * -- rd = Success code.
 * -- rm = Parameter x index.
 * -- ra = Parameter value.
 * -- rn = Parameter y index.
 */
uint64_t smmParamWrite(uint64_t rm, uint64_t rn, uint64_t ra)
{
    uint64_t res;

    __asm__ volatile(
            "MOV X8, %[input_i];"
            "MOV X9, %[input_j];"
            "MOV X7, %[input_k];"
            ".long 0x41081D2A;"
            "MOV %[output], X10;"
            : [output] "=r" (res)
            : [input_i] "r" (ra), [input_j] "r" (rm), [input_k] "r" (rn)
            : "x7", "x8", "x9", "x10"
            );

    return res;

}


void smmCompute(std::size_t seq_len, const uint32_t *input, uint32_t *output, uint32_t *weights,
                             std::size_t input_size_, std::size_t output_size_) {

    for (int tileCol = 0; tileCol < output_size_ / KERNEL_DIM; tileCol++) {
        std::cout << "Tile Column : " << tileCol << std::endl;
        for (int tileRow = 0; tileRow < input_size_ / KERNEL_DIM; tileRow++) {
            // Load the kernel with the corresponding weight
            int rowStart = tileRow * KERNEL_DIM;
            int colStart = tileCol * KERNEL_DIM / W_DATA;
            int rowBlockSize = KERNEL_DIM;
            int colBlockSize = KERNEL_DIM / W_DATA;
            for (int i = rowStart; i < rowStart + rowBlockSize; i++) {
                for (int j = colStart; j < colStart + colBlockSize; j++) {
                    uint32_t weight = mem2d(weights, output_size_ / W_DATA, i, j);
                    smmParamWrite(i - rowStart, j - colStart, weight);
                }
            }

            // Process the multiplication
            int base_col_idx = tileRow * MAX_COL;
            int outputIndex = 0;
            uint32_t mult;
            for (int i = 0; i < seq_len; i++) {
                for (int j = 0; j < MAX_COL; j++) {
                    if (j == MAX_COL - 1) {
                        mult = smmStream(mem2d(input, input_size_ / W_DATA, i, j + base_col_idx));
                    } else {
                        mult = smmQueue(j % MAX_COL, mem2d(input, input_size_ / W_DATA, i, j + base_col_idx));
                    }

                    if ((i * MAX_COL + j) >= (MAX_COL * (2 * KERNEL_DIM - 1) - 1)) {    // check if the output is valid
                        add8in32(mem2d(output, output_size_ / W_DATA, outputIndex / colBlockSize,
                                       colStart + outputIndex % colBlockSize), mult);
                        outputIndex++;
                    }
                }
            }
            for (int i = seq_len * MAX_COL; i < MAX_COL * (seq_len + 2 * KERNEL_DIM - 1) - 1; i++) {
                if ((i % MAX_COL) == MAX_COL - 1) {
                    mult = smmStream(0);
                } else {
                    mult = smmQueue(i % MAX_COL, 0);
                }
                if (i >= (MAX_COL * (2 * KERNEL_DIM - 1) - 1)) { // check if the output is valid
                    add8in32(mem2d(output, output_size_ / W_DATA, outputIndex / colBlockSize,
                                   colStart + outputIndex % colBlockSize), mult);
                    outputIndex++;
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
void print_arr(uint32_t* array, int n, int p){
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < p / W_DATA; j++)
            std::cout << std::hex << (uint32_t) mem2d(array, p / W_DATA, i, j) << "\t";
        std::cout << std::endl;
    }
    std::cout << std::endl;
}



void conventionalCompute(std::size_t seq_len, const uint32_t * input, uint32_t * output, uint32_t *weight,
                        std::size_t input_size_, std::size_t output_size_){
    int ROW_BLOCKS = 16;
    int COL_BLOCKS = 16;

    for (int blk_row_idx=0; blk_row_idx< (input_size_/ROW_BLOCKS); blk_row_idx++){
        for (int blk_col_idx=0; blk_col_idx < (output_size_/ COL_BLOCKS); blk_col_idx ++){
            for (int in_idx = 0; in_idx < seq_len; in_idx++){
                for (int col=0; col< COL_BLOCKS; col++){
                    auto* input_ptr = (int8_t*) (input +
                            (in_idx * input_size_ / W_DATA) +   // index of the input row
                            blk_row_idx * ROW_BLOCKS/W_DATA);   // block index
                            auto* output_ptr = (int8_t*) (output +
                                    (in_idx * output_size_ / W_DATA) +
                                    blk_col_idx * COL_BLOCKS/ W_DATA);
                            auto* weight_ptr = (int8_t*) (weight +
                                    blk_row_idx * ROW_BLOCKS * output_size_ / W_DATA +
                                    blk_col_idx * COL_BLOCKS/W_DATA);
                            int sum = 0;
                            for (int i = 0; i < ROW_BLOCKS; i ++){
                                sum += *(input_ptr + i) * *(weight_ptr + i * output_size_ + col);
                            }
                            * (output_ptr  + col)= (int8_t)((* (output_ptr+ col)) + sum);
                }
            }


        }
    }
}



