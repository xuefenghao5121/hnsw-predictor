// 精确对比: 统计 visited 节点数差异
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

    // F0 baseline
    DiskHNSW f0(argv[1],argv[2],argv[3],argv[4],2225,dim);
    f0.setEf(ef);
    for(int i=0;i<50&&i<nq;i++) f0.searchKnn(&queries[(size_t)i*dim],k);

    std::vector<std::set<uint64_t>> f0_sets(nq);
    for(int i=0;i<nq;i++){
        auto r=f0.searchKnn(&queries[(size_t)i*dim],k);
        for(auto&x:r) f0_sets[i].insert(x.second);
    }

    // Non-blocking
    DiskHNSW nb(argv[1],argv[2],argv[3],argv[4],1024,dim);
    nb.setEf(ef); nb.enableGraphPrefetch(true);
    for(int i=0;i<50&&i<nq;i++){ std::vector<float>q(&queries[(size_t)i*dim],&queries[(size_t)i*dim]+dim); nb.batchSearch(q,k,1); }

    int total_match=0, total=nq*k;
    int mismatches=0;
    for(int i=0;i<nq;i++){
        std::vector<float>q(&queries[(size_t)i*dim],&queries[(size_t)i*dim]+dim);
        auto r=nb.batchSearch(q,k,1);
        std::set<uint64_t> nbs;
        for(auto&x:r[0]) nbs.insert(x.second);
        for(auto l:nbs) if(f0_sets[i].count(l)) total_match++;
        if(nbs!=f0_sets[i]){
            mismatches++;
            if(mismatches<=3){
                std::vector<uint64_t> miss,extra;
                for(auto l:f0_sets[i]) if(!nbs.count(l)) miss.push_back(l);
                for(auto l:nbs) if(!f0_sets[i].count(l)) extra.push_back(l);
                std::cout<<"Q"<<i<<": miss={";for(auto l:miss)std::cout<<l<<" ";std::cout<<"} extra={";for(auto l:extra)std::cout<<l<<" ";std::cout<<"}\n";
            }
        }
    }
    std::cout<<"\nNon-blocking recall@HNSW: "<<(100.0*total_match/total)<<"% (mismatch="<<mismatches<<"/"<<nq<<")\n";
    return 0;
}
