//
// Created by alireza on 3/2/22.
//

#include "transformerBlock.h"

void print_out(uint32_t* output_array, std::size_t width , std::size_t seq_len){
    for (int i = 0; i < seq_len; i++) {
        for (int j = 0; j < width >> 2; j++)
            std::cout << std::hex << (uint32_t) output_array[i*(width >> 2) + j] << "\t";
        std::cout << std::endl;
    }
}


TransformerBlock::TransformerBlock(std::size_t pre_seq_len, std::size_t input_dim, std::size_t head_hidden_size,
                                   std::size_t num_heads, std::size_t ff_size, uint32_t ** weightVector,
                                   std::size_t kernelDim, std::size_t maxCol) {

    num_heads_ = num_heads;
    head_hidden_size_ = head_hidden_size;
    ff_size_ = ff_size;

    for (int n =0; n< num_heads; n++){
        selfatten[n] = new SingleHeadSelfAttn(pre_seq_len, input_dim, head_hidden_size, weightVector+n*3);
    }

    condense = new Dense(num_heads* head_hidden_size, input_dim, weightVector[num_heads * 3]);

    multihead_out = new uint32_t[pre_seq_len * num_heads * head_hidden_size >> 2];
    condense_out = new uint32_t[pre_seq_len * input_dim >> 2];
    intermediateFF = new uint32_t[pre_seq_len * ff_size >> 2];

    addNorm = new AddNormalize(pre_seq_len, input_dim, kernelDim, maxCol);
    feedForward0 = new Dense(input_dim, ff_size, weightVector[num_heads * 3+ 1]);
    feedForward1 = new Dense(ff_size, input_dim, weightVector[num_heads * 3 + 2]);
}

TransformerBlock::~TransformerBlock() = default;

void print_weight(uint32_t* kernel, int n_row, int n_col){
    for (int i=0; i<n_row; i++){
        for (int j=0; j<n_col; j++){
            printf("%08x\t", kernel[i*n_col + j]);
        }
        printf("\n");
    }
}

void TransformerBlock::compute(std::size_t seq_len, uint32_t *input, uint32_t *output) {
    system("m5 resetstats");
    for (int n=0; n<num_heads_; n++){
        std::cout << "Head : " << n << std::endl;
        selfatten[n]->compute(seq_len, input, multihead_out + n * (seq_len * head_hidden_size_ >> 2));
    }
    system("m5 dumpresetstats");

    std::cout << "Condense"  << std::endl;
    condense->compute(seq_len, multihead_out, condense_out);
    system("m5 dumpresetstats");

    std::cout<< "Before Add/Norm: " << std::endl;
    print_weight(condense_out, seq_len * head_hidden_size_ * num_heads_/64, 8* 2);

    std::cout << "Add Norm"  << std::endl;
    addNorm->computeRearranged(input, condense_out);

    std::cout<< "After Add/Norm: " << std::endl;
    print_weight(condense_out, seq_len * head_hidden_size_ * num_heads_/64, 8* 2);
    getchar();
    system("m5 dumpresetstats");

    std::cout << "Feed Forward 0"  << std::endl;
    feedForward0->compute(seq_len, condense_out, intermediateFF);
    system("m5 dumpresetstats");

    std::cout << "Feed Forward 1"  << std::endl;
    feedForward1->compute(seq_len, intermediateFF, output);
    system("m5 dumpresetstats");

    std::cout << "Add Norm"  << std::endl;
//    addNorm->compute(condense_out, output);
    system("m5 dumpresetstats");
}
