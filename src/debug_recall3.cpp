// 对比: 同一实例, 先跑 searchKnn 再跑 searchLayer0NonBlocking (通过 batchSearch)
// 消除缓存状态差异的影响
#include "disk_hnsw.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <set>

std::vector<float> read_fvecs(const std::string& path, int& dim, int& num) {
    std::ifstream f(path, std::ios::binary);
    f.read(reinterpret_cast<char*>(&dim), 4);
    f.seekg(0, std::ios::end);
    auto sz = f.tellg(); f.seekg(0, std::ios::beg);
    num = sz / (4 + 4*dim);
    std::vector<float> d((size_t)num*dim); int tmp;
    for (int i = 0; i < num; i++) { f.read((char*)&tmp,4); f.read((char*)&d[(size_t)i*dim],4LL*dim); }
    return d;
}

int main(int argc, char** argv) {
    int k = argc>8?atoi(argv[8]):10, ef = argc>9?atoi(argv[9]):50, nq = argc>10?atoi(argv[10]):200;
    int dim, nbase, nquery;
    auto queries = read_fvecs(argv[6], dim, nquery);
    nq = std::min(nq, nquery);

    // F0 baseline (独立实例)
    DiskHNSW f0(argv[1],argv[2],argv[3],argv[4],2225,dim);
    f0.setEf(ef);
    for(int i=0;i<50&&i<nq;i++) f0.searchKnn(&queries[(size_t)i*dim],k);

    std::vector<std::set<uint64_t>> f0_sets(nq);
    for(int i=0;i<nq;i++){
        auto r=f0.searchKnn(&queries[(size_t)i*dim],k);
        for(auto&x:r) f0_sets[i].insert(x.second);
    }

    // 同一实例: 先跑 searchKnn (阻塞), 再跑 batchSearch (非阻塞)
    DiskHNSW hnsw(argv[1],argv[2],argv[3],argv[4],1024,dim);
    hnsw.setEf(ef);
    hnsw.enableGraphPrefetch(true);

    // warmup
    for(int i=0;i<50&&i<nq;i++) hnsw.searchKnn(&queries[(size_t)i*dim],k);

    // 1. 阻塞版
    std::cout << "Blocking...\n";
    std::vector<std::set<uint64_t>> block_sets(nq);
    for(int i=0;i<nq;i++){
        auto r=hnsw.searchKnn(&queries[(size_t)i*dim],k);
        for(auto&x:r) block_sets[i].insert(x.second);
    }
    int block_match=0;
    for(int i=0;i<nq;i++) for(auto l:block_sets[i]) if(f0_sets[i].count(l)) block_match++;
    std::cout << "Blocking recall@HNSW: " << (100.0*block_match/(nq*k)) << "%\n";

    // 2. 非阻塞版 (同一实例, 缓存状态延续)
    std::cout << "Non-blocking (same instance)...\n";
    std::vector<std::set<uint64_t>> nb_sets(nq);
    for(int i=0;i<nq;i++){
        std::vector<float> q(&queries[(size_t)i*dim], &queries[(size_t)i*dim]+dim);
        auto r=hnsw.batchSearch(q,k,1);
        for(auto&x:r[0]) nb_sets[i].insert(x.second);
    }
    int nb_match=0, nb_mismatch=0;
    for(int i=0;i<nq;i++){
        for(auto l:nb_sets[i]) if(f0_sets[i].count(l)) nb_match++;
        if(nb_sets[i]!=f0_sets[i]) nb_mismatch++;
    }
    std::cout << "Non-blocking recall@HNSW: " << (100.0*nb_match/(nq*k)) << "% (mismatch=" << nb_mismatch << "/" << nq << ")\n";

    // 3. 再跑一次阻塞版 (验证缓存状态是否导致差异)
    std::cout << "Blocking again (same instance)...\n";
    std::vector<std::set<uint64_t>> block2_sets(nq);
    for(int i=0;i<nq;i++){
        auto r=hnsw.searchKnn(&queries[(size_t)i*dim],k);
        for(auto&x:r) block2_sets[i].insert(x.second);
    }
    int block2_match=0, block2_mm=0;
    for(int i=0;i<nq;i++){
        for(auto l:block2_sets[i]) if(f0_sets[i].count(l)) block2_match++;
        if(block2_sets[i]!=f0_sets[i]) block2_mm++;
    }
    std::cout << "Blocking (2nd) recall@HNSW: " << (100.0*block2_match/(nq*k)) << "% (mismatch=" << block2_mm << "/" << nq << ")\n";

    return 0;
}
