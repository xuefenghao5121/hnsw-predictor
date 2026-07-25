// build_index.cpp - 构建 HNSW 索引
// 用途: 读取 fvecs 数据，用 hnswlib 构建 HNSW 索引，保存为 .bin 文件
// 
// 用法: ./build_index <data.fvecs> <index.bin> [M] [ef_construction]
// 默认: M=16, ef_construction=200

#include "hnswlib/hnswlib.h"
#include "common.h"
#include <chrono>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <data.fvecs> <index.bin> [M] [ef_construction]" << std::endl;
        return 1;
    }
    
    std::string data_path = argv[1];
    std::string index_path = argv[2];
    size_t M = (argc > 3) ? std::stoul(argv[3]) : 16;
    size_t ef_construction = (argc > 4) ? std::stoul(argv[4]) : 200;
    
    // 读取数据
    std::cout << "Reading data from " << data_path << "..." << std::endl;
    int dim;
    size_t num_elements;
    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<float> data = read_fvecs(data_path, dim, num_elements);
    auto t1 = std::chrono::high_resolution_clock::now();
    std::cout << "  Data: " << num_elements << " vectors, dim=" << dim 
              << ", " << std::chrono::duration<double>(t1 - t0).count() << "s" << std::endl;
    
    // 创建空间和索引
    hnswlib::L2Space space(dim);
    auto* alg_hnsw = new hnswlib::HierarchicalNSW<float>(&space, num_elements, M, ef_construction);
    
    // 构建索引
    std::cout << "Building HNSW index (M=" << M << ", ef_construction=" << ef_construction << ")..." << std::endl;
    t0 = std::chrono::high_resolution_clock::now();
    
    for (size_t i = 0; i < num_elements; i++) {
        alg_hnsw->addPoint(&data[i * dim], i);
        if ((i + 1) % 100000 == 0) {
            std::cout << "  Inserted " << (i + 1) << "/" << num_elements << "..." << std::endl;
        }
    }
    
    t1 = std::chrono::high_resolution_clock::now();
    std::cout << "  Build complete in " << std::chrono::duration<double>(t1 - t0).count() << "s" << std::endl;
    
    // 保存索引
    std::cout << "Saving index to " << index_path << "..." << std::endl;
    alg_hnsw->saveIndex(index_path);
    
    // 输出统计信息
    std::cout << "\n=== Index Statistics ===" << std::endl;
    std::cout << "  Elements: " << alg_hnsw->getCurrentElementCount() << std::endl;
    std::cout << "  Max elements: " << alg_hnsw->getMaxElements() << std::endl;
    std::cout << "  M: " << M << std::endl;
    std::cout << "  ef_construction: " << ef_construction << std::endl;
    
    delete alg_hnsw;
    std::cout << "Done." << std::endl;
    return 0;
}
