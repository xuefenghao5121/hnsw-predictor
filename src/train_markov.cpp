// train_markov.cpp - Task 3.2: 离线训练 Markov 模型
//
// 用法:
//   ./train_markov <trace_file> <model_output.bin>
#include "predictor.h"
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <trace_file> <model_output.bin>" << std::endl;
        return 1;
    }

    std::string trace_path = argv[1];
    std::string model_path = argv[2];

    MarkovPredictor predictor;
    predictor.trainFromTraces(trace_path);
    predictor.printSummary();
    predictor.saveModel(model_path);

    std::cout << "Done. Model saved to " << model_path << std::endl;
    return 0;
}
