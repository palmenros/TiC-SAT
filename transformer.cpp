#include"transformer_layers/transformerBlock.h"
//#include"gtest/gtest.h"
#include "transformer.h"
#include "accelerator/smm_gem.h"
#include <fstream>

#ifndef RELOAD_WEIGHT
#include <filesystem>
#endif

#include "transformer_layers/debuggerFunctions.h"

#define KERNEL_DIM SA_SIZE
#define MAX_COL (SA_SIZE/4)


void fill_kernel(uint32_t *kernel, int kernel_size) {
    for (int i = 0; i < kernel_size; i++) {
        uint32_t result = 0;
        for (int j = 0; j < 4; j++) {
            result |= ((uint8_t) (rand() % 5 - 2)) << (8 * j);
        }
        kernel[i] = result;
    }
}

void print_binary(uint32_t value) {
    for (int i = 31; i >= 0; i--) {
        std::cout << ((value >> i) & 1);
    }
    std::cout << std::endl;
}

void fill_sparse_weight(uint32_t *kernel, int n_row, int n_col) {
    uint32_t *kernel_ptr = kernel;
    for (int i = 0; i < n_row / KERNEL_DIM; i++) {
        for (int j = 0; j < n_col / MAX_COL; j++) {
            for (int ii = 0; ii < KERNEL_DIM; ii++) {
                for (int jj = 0; jj < MAX_COL; jj++) {
                    uint32_t result = 0;
                    for (int k = 0; k < 4; k++) {
                        result |= ((uint8_t) (rand() % 5 - 2)) << (8 * k);
                    }
#ifdef REARRANGE
                    *kernel_ptr = result;
                    kernel_ptr++;
#else
                    kernel[(i*KERNEL_DIM+ii)*n_col + (j*MAX_COL + jj)]=result;
#endif
                }
            }
        }
    }
}

void saveWeight(int n_head, int qkv, int size, uint32_t *array, const std::string &dir_name) {
    // Write the kernel array to file
    std::string filename = dir_name + "/H" + std::to_string(n_head) + "_L" +
                           std::to_string(qkv) + ".bin";
    std::ofstream fout(filename);
    if (fout.is_open()) {
        for (int i = 0; i < size; i++) {
            fout << array[i] << " ";
        }
        fout.close();
    }
}

void loadWeight(int n_head, int qkv, int size, uint32_t *array, const std::string &dir_name) {
    std::string filename = dir_name + "/H" + std::to_string(n_head) + "_L" +
            std::to_string(qkv) + "_S0.bin";
    std::ifstream fin(filename);
    if (fin.is_open()) {
        for (int i = 0; i < size; i++) {
            fin >> array[i];
        }
        fin.close();
    } else {
        std::cout << filename + " Not loaded" << std::endl;
    }
}

void test() {
    std::cout << "Welcome to TiC-SAT" << std::endl;
#ifdef REARRANGE
    std::cout << "BWMA method" << std::endl;
#else
    std::cout<<"RWMA method" << std::endl;
#endif

    std::string dir_name = "/home/alireza/CLionProjects/FvllMontiTransformer/data16";

    uint32_t *tensor_in = new uint32_t[D_SEQ * D_MODEL >> 2];
#ifdef RELOAD_WEIGHT
    // Load the tensor input from file
    // We assign -1 and -1 to n_head and qkv to indicate that we are not loading the weight
    loadWeight(-1, -1, D_SEQ * D_MODEL >> 2, tensor_in, dir_name);
#else
    fill_kernel(tensor_in, D_SEQ * D_MODEL >> 2);
    // Save the tensor input to file
    // We assign -1 and -1 to n_head and qkv to indicate that we are not saving the weight
    saveWeight(-1, -1, D_SEQ * D_MODEL >> 2, tensor_in, 0, dir_name);

    std::ofstream outfile("flags_generated.h");
    outfile << "#pragma once" << std::endl;
    outfile << std::endl;
    outfile << "#include <cstdint>" << std::endl;
    outfile << std::endl;
    outfile << "static const uint32_t flags[] = {";
    outfile.close();
#endif

#ifndef REARRANGE
    uint32_t tensorInRowWise[D_SEQ * D_MODEL >> 2];
    // By default, the saved tensor is in block-wise format
    // We need to convert it to row-wise format
    blockWise2RowWise(tensor_in, tensorInRowWise, D_SEQ, D_MODEL >> 2);
    tensor_in = tensorInRowWise;
#endif

    uint32_t *out = new uint32_t[D_SEQ * D_MODEL >> 2]();
    uint32_t *weightVec[3 * NUM_HEAD + 3];
    int head_qkv_size = D_Q * D_MODEL >> 2;

    for (int n = 0; n < NUM_HEAD; n++) {
        volatile auto query_kernel = new uint32_t[D_Q * D_MODEL >> 2]();
        volatile auto key_kernel = new uint32_t[D_Q * D_MODEL >> 2]();
        volatile auto value_kernel = new uint32_t[D_Q * D_MODEL >> 2]();

#ifdef RELOAD_WEIGHT
        loadWeight(n, 0, head_qkv_size, query_kernel, dir_name);
        loadWeight(n, 1, head_qkv_size, key_kernel, dir_name);
        loadWeight(n, 2, head_qkv_size, value_kernel, dir_name);
#else
        fill_sparse_weight(query_kernel, query_flag, D_MODEL, D_Q >> 2, sparsity_percentage);
        fill_sparse_weight(key_kernel, key_flag, D_MODEL, D_Q >> 2, sparsity_percentage);
        fill_sparse_weight(value_kernel, value_flag, D_MODEL, D_Q >> 2, sparsity_percentage);

        saveWeight(n, 0, head_qkv_size, query_kernel, sparsity_percentage, dir_name);
        saveWeight(n, 1, head_qkv_size, key_kernel, sparsity_percentage, dir_name);
        saveWeight(n, 2, head_qkv_size, value_kernel, sparsity_percentage, dir_name);

        saveWeight(n, 10, head_flag_size, query_flag, sparsity_percentage, dir_name);
        saveWeight(n, 11, head_flag_size, key_flag, sparsity_percentage, dir_name);
        saveWeight(n, 12, head_flag_size, value_flag, sparsity_percentage, dir_name);
        append_flags(query_flag, head_flag_size);
        append_flags(key_flag, head_flag_size);
        append_flags(value_flag, head_flag_size);
#endif

#ifndef REARRANGE
        // By default, the saved weights are in block-wise format
        // We need to convert them to row-wise format
        uint32_t* queryRowWise = new uint32_t [D_MODEL * D_Q >> 2];
        blockWise2RowWise(query_kernel, queryRowWise, D_MODEL, D_Q >> 2);
        query_kernel = queryRowWise;
        uint32_t* keyRowWise = new uint32_t [D_MODEL * D_Q >> 2];
        blockWise2RowWise(key_kernel, keyRowWise, D_MODEL, D_Q >> 2);
        key_kernel = keyRowWise;
        uint32_t* valueRowWise = new uint32_t [D_MODEL * D_Q >> 2];
        blockWise2RowWise(value_kernel, valueRowWise, D_MODEL, D_Q >> 2);
        value_kernel = valueRowWise;
#endif

        weightVec[n * 3] = query_kernel;
        weightVec[n * 3 + 1] = key_kernel;
        weightVec[n * 3 + 2] = value_kernel;
    }

    volatile auto condense_kernel = new uint32_t[NUM_HEAD * D_Q * D_MODEL >> 2]();
    volatile auto ff0_kernel = new uint32_t[D_MODEL * D_FF >> 2]();
    volatile auto ff1_kernel = new uint32_t[D_FF * D_MODEL >> 2]();

#ifdef RELOAD_WEIGHT
    int n = -1; // n=-1 means that we are not saving/loading a head

    loadWeight(n, 0, NUM_HEAD * D_Q * D_MODEL >> 2, condense_kernel, dir_name);
    loadWeight(n, 1, D_MODEL * D_FF >> 2, ff0_kernel, dir_name);
    loadWeight(n, 2, D_MODEL * D_FF >> 2, ff1_kernel, dir_name);
#else
    fill_sparse_weight(condense_kernel, condense_flag, D_MODEL, NUM_HEAD * D_Q >> 2, sparsity_percentage);
    fill_sparse_weight(ff0_kernel, ff0_flag, D_MODEL, D_FF >> 2, sparsity_percentage);
    fill_sparse_weight(ff1_kernel, ff1_flag, D_FF, D_MODEL >> 2, sparsity_percentage);

    int n = -1; // n=-1 means that we are not saving/loading a head
    saveWeight(n, 0, NUM_HEAD * D_Q * D_MODEL >> 2, condense_kernel, sparsity_percentage, dir_name);
    saveWeight(n, 1, D_MODEL* D_FF >> 2, ff0_kernel, sparsity_percentage, dir_name);
    saveWeight(n, 2, D_MODEL* D_FF >> 2, ff1_kernel, sparsity_percentage, dir_name);

    saveWeight(n, 10, NUM_HEAD * D_Q * D_MODEL / (32 * KERNEL_DIM * MAX_COL), condense_flag, sparsity_percentage, dir_name);
    saveWeight(n, 11, D_MODEL* D_FF / (32* KERNEL_DIM * MAX_COL), ff0_flag, sparsity_percentage, dir_name);
    saveWeight(n, 12, D_MODEL* D_FF / (32* KERNEL_DIM * MAX_COL), ff1_flag, sparsity_percentage, dir_name);

    append_flags(condense_flag, NUM_HEAD * D_Q * D_MODEL / (32 * KERNEL_DIM * MAX_COL));
    append_flags(ff0_flag, D_MODEL* D_FF / (32* KERNEL_DIM * MAX_COL));
    append_flags(ff1_flag, D_MODEL* D_FF / (32* KERNEL_DIM * MAX_COL));

    outfile.open("flags_generated.h", std::ios::app);
    outfile << "};" << std::endl;
    outfile.close();
#endif

#ifndef REARRANGE
    // By default, the saved weights are in block-wise format
    // We need to convert them to row-wise format
    uint32_t* condenseRowWise = new uint32_t [NUM_HEAD * D_Q * D_MODEL >> 2];
    blockWise2RowWise(condense_kernel, condenseRowWise, NUM_HEAD * D_Q, D_MODEL >> 2);
    condense_kernel = condenseRowWise;
    uint32_t* ff0RowWise = new uint32_t [D_MODEL * D_FF >> 2];
    blockWise2RowWise(ff0_kernel, ff0RowWise, D_MODEL, D_FF >> 2);
    ff0_kernel = ff0RowWise;
    uint32_t* ff1RowWise = new uint32_t [D_FF * D_MODEL >> 2];
    blockWise2RowWise(ff1_kernel, ff1RowWise, D_FF, D_MODEL >> 2);
    ff1_kernel = ff1RowWise;
#endif

    weightVec[NUM_HEAD * 3] = condense_kernel;
    weightVec[NUM_HEAD * 3 + 1] = ff0_kernel;
    weightVec[NUM_HEAD * 3 + 2] = ff1_kernel;

    TransformerBlock selfatten(D_SEQ, D_MODEL, D_Q, NUM_HEAD, D_FF, weightVec, KERNEL_DIM, MAX_COL);
    selfatten.compute(D_SEQ, tensor_in, out);
}

int main() {
    test();
    return 0;
}