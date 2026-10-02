// cxx-test.cpp - C++23 on QRT (libc++, libc++abi, libunwind, compiler-rt built for QRT's
// musl): exceptions through several frames, threads and atomics, <format>, <filesystem>,
// iostreams, containers, std::expected, coroutines, chrono.  Prints "c++23: ok".
#include <atomic>
#include <chrono>
#include <coroutine>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

static int fails;
static void check(bool ok, std::string_view what) {
    std::cout << std::format("c++23: {} {}\n", what, ok ? "ok" : "FAILED");
    if (!ok) fails++;
}

[[noreturn]] static void deep(int n) { if (n == 0) throw std::runtime_error("from deep down"); deep(n - 1); for (;;) {} }

struct Gen {
    struct promise_type {
        int value = 0;
        Gen get_return_object() { return Gen { std::coroutine_handle<promise_type>::from_promise(*this) }; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(int v) { value = v; return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
    std::coroutine_handle<promise_type> h;
    ~Gen() { if (h) h.destroy(); }
};
static Gen count_to(int n) { for (int i = 1; i <= n; i++) co_yield i; }

static std::expected<int, std::string> parse(std::string_view s) {
    if (s.empty()) return std::unexpected("empty");
    return static_cast<int>(s.size());
}

int main() {
    std::string caught;
    try { deep(20); } catch (const std::exception& e) { caught = e.what(); }
    check(caught == "from deep down", "exceptions");

    std::atomic<int> n { 0 };
    std::mutex mu;
    std::vector<int> order;
    {
        std::vector<std::jthread> ts;
        for (int i = 0; i < 4; i++)
            ts.emplace_back([&, i] { for (int k = 0; k < 10000; k++) n++; std::lock_guard g(mu); order.push_back(i); });
    }
    check(n == 40000 && order.size() == 4, "threads, atomics, jthread");

    namespace fs = std::filesystem;
    fs::create_directories("/tmp/cxx/a/b");
    { std::ofstream("/tmp/cxx/a/b/f.txt") << "hello " << 42; }
    std::ifstream in("/tmp/cxx/a/b/f.txt");
    std::string w; int v = 0;
    in >> w >> v;
    int files = 0;
    for (auto& e : fs::recursive_directory_iterator("/tmp/cxx")) files += e.is_regular_file();
    check(w == "hello" && v == 42 && files == 1 && fs::file_size("/tmp/cxx/a/b/f.txt") == 8, "filesystem, fstream");
    fs::remove_all("/tmp/cxx");

    std::map<std::string, std::variant<int, std::string>> m { { "a", 1 }, { "b", std::string("two") } };
    auto squares = std::views::iota(1, 6) | std::views::transform([](int x) { return x * x; });
    int sum = 0;
    for (int s : squares) sum += s;
    check(std::get<std::string>(m["b"]) == "two" && sum == 55 && std::format("{:>5}|{:.2f}", 42, 3.14159) == "   42|3.14", "containers, ranges, format");

    check(parse("abc").value() == 3 && !parse("").has_value() && parse("").error() == "empty", "std::expected");

    Gen g = count_to(5);
    int total = 0;
    while (true) { g.h.resume(); if (g.h.done()) break; total += g.h.promise().value; }
    check(total == 15, "coroutines");

    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(19), "chrono");

    auto up = std::make_unique<std::optional<int>>(7);
    std::shared_ptr<int> sp = std::make_shared<int>(9);
    check(**up == 7 && *sp == 9 && sp.use_count() == 1, "smart pointers");

    std::cout << (fails ? "c++23: FAILED\n" : "c++23: ok\n");
    return fails ? 1 : 0;
}
