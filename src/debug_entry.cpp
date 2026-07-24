// 精简版: 只跑3个query, 对比 searchKnn vs batchSearch(bs=1) 的 entry_new_id
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
    int dim, nbase, nquery;
    auto queries = read_fvecs(argv[6], dim, nquery);
    int nq = 200;

    // F0 baseline
    DiskHNSW f0(argv[1],argv[2],argv[3],argv[4],2225,dim);
    f0.setEf(50);
    for(int i=0;i<50&&i<nquery;i++) f0.searchKnn(&queries[(size_t)i*dim],10);
    std::vector<std::set<uint64_t>> f0_sets(nq);
    for(int i=0;i<nq;i++){
        auto r=f0.searchKnn(&queries[(size_t)i*dim],10);
        for(auto&x:r) f0_sets[i].insert(x.second);
    }

    // Test instance
    DiskHNSW hnsw(argv[1],argv[2],argv[3],argv[4],1024,dim);
    hnsw.setEf(50);
    hnsw.enableGraphPrefetch(true);
    // warmup
    for(int i=0;i<50&&i<nquery;i++) hnsw.searchKnn(&queries[(size_t)i*dim],10);

    // searchKnn
    for(int i=0;i<nq;i++){
        auto r=hnsw.searchKnn(&queries[(size_t)i*dim],10);
        std::set<uint64_t> s;
        for(auto&x:r) s.insert(x.second);
        std::cout << "Q" << i << " SK match=" << [&](){
            int c=0; for(auto l:s) if(f0_sets[i].count(l)) c++; return c;
        }() << "/" << 10 << "\n";
    }

    std::cout << "---\n";

    // batchSearch(bs=1)
    for(int i=0;i<nq;i++){
        std::vector<float> q(&queries[(size_t)i*dim], &queries[(size_t)i*dim]+dim);
        auto r=hnsw.batchSearch(q,10,1);
        std::set<uint64_t> s;
        for(auto&x:r[0]) s.insert(x.second);
        std::cout << "Q" << i << " BS match=" << [&](){
            int c=0; for(auto l:s) if(f0_sets[i].count(l)) c++; return c;
        }() << "/" << 10 << "\n";
    }

    return 0;
}
