// cxx.cpp - C++ on QRT: libstdc++, exceptions, std::thread, std::mutex,
// containers and iostreams, dynamically linked.  Prints "c++: ok".
#include <algorithm>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

int main() {
    std::map<std::string, int> words;
    for (std::string w : {"tessera", "qrt", "linux", "qrt"}) words[w]++;
    std::mutex m;
    long total = 0;
    std::vector<std::thread> pool;
    for (int i = 0; i < 3; i++)
        pool.emplace_back([&, i] { for (int k = 0; k < 1000; k++) { std::lock_guard<std::mutex> g(m); total += i + 1; } });
    for (auto &t : pool) t.join();
    bool caught = false;
    try { throw std::runtime_error("unwinding works"); } catch (const std::exception &e) { caught = true; std::cout << "c++: " << e.what() << "\n"; }
    std::vector<int> v{5, 3, 9, 1};
    std::sort(v.begin(), v.end());
    bool good = words["qrt"] == 2 && total == 6000 && caught && v.front() == 1;
    std::cout << "c++: " << words.size() << " words, total " << total << ", " << (good ? "ok" : "FAILED") << std::endl;
    return good ? 0 : 1;
}
