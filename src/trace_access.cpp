// 对比非阻塞和阻塞版的访问路径差异
#include "disk_hnsw.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <set>
#include <map>

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

    // Blocking instance
    DiskHNSW hnsw_b(argv[1],argv[2],argv[3],argv[4],1024,dim);
    hnsw_b.setEf(50);
    hnsw_b.enableGraphPrefetch(false);  // disable to use blocking

    // Warmup
    for(int i=0;i<50&&i<nquery;i++) hnsw_b.searchKnn(&queries[(size_t)i*dim],10);

    // Run blocking search and trace access
    auto r_b = hnsw_b.searchKnn(&queries[0],10);
    std::set<uint64_t> blocking_set;
    for(auto&x:r_b) blocking_set.insert(x.second);

    std::cout << "Blocking top-10: ";
    for(auto l:blocking_set) std::cout << l << " ";
    std::cout << "\n";

    // Non-blocking instance
    DiskHNSW hnsw_nb(argv[1],argv[2],argv[3],argv[4],1024,dim);
    hnsw_nb.setEf(50);
    hnsw_nb.enableGraphPrefetch(true);

    // Warmup
    for(int i=0;i<50&&i<nquery;i++) hnsw_nb.searchKnn(&queries[(size_t)i*dim],10);

    // Run non-blocking batch search (bs=1)
    std::vector<float> q(&queries[0], &queries[dim]);
    auto r_nb = hnsw_nb.batchSearch(q,10,1);
    std::set<uint64_t> nonblocking_set;
    for(auto&x:r_nb[0]) nonblocking_set.insert(x.second);

    std::cout << "Non-blocking top-10: ";
    for(auto l:nonblocking_set) std::cout << l << " ";
    std::cout << "\n";

    // Find intersection
    std::set<uint64_t> intersect;
    for(auto l:blocking_set) if(nonblocking_set.count(l)) intersect.insert(l);

    std::cout << "Intersection: " << intersect.size() << "/10\n";
    std::cout << "Blocking only: ";
    for(auto l:blocking_set) if(!nonblocking_set.count(l)) std::cout << l << " ";
    std::cout << "\nNon-blocking only: ";
    for(auto l:nonblocking_set) if(!blocking_set.count(l)) std::cout << l << " ";
    std::cout << "\n";

    return 0;
}