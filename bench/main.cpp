// ============================================
// bench/main.cpp — hft_bench entry point (hft_bench 入口)
// Usage (用法):
//   hft_bench --output docs/BENCHMARK.md --runs 5 --warmup 1 --format md
//   hft_bench --list
//   hft_bench --filter order_manager
// ============================================

#include "bench/bench_runner.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

using namespace hft::bench;

namespace {

struct CliArgs {
    std::string output_path;        // empty = stdout (为空表示输出到 stdout)
    std::string format = "md";      // md | csv | json (输出格式)
    std::string filter;             // regex matching case name; empty = all (正则匹配 case name; 空 = 全跑)
    BenchConfig cfg{};
    bool list_only = false;
    bool show_help = false;
};

void print_help() {
    std::cout <<
        "hft_bench — hft_framework latency benchmark runner\n\n"
        "Usage:\n"
        "  hft_bench [options]\n\n"
        "Options:\n"
        "  --output <path>     输出文件路径(默认 stdout)\n"
        "  --format <md|csv|json>  输出格式,默认 md\n"
        "  --runs <N>          每个场景重复轮数(取最稳定一轮),默认 3\n"
        "  --warmup <N>        预热轮数(丢弃),默认 1\n"
        "  --samples <N>       延迟场景每轮采样数,默认 10000\n"
        "  --filter <regex>    只跑名字匹配该正则的场景\n"
        "  --list              列出所有已注册场景后退出\n"
        "  --help              显示本帮助\n";
}

bool parse_args(int argc, char** argv, CliArgs& out) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") {
            out.show_help = true;
        } else if (a == "--list") {
            out.list_only = true;
        } else if (a == "--output") {
            const char* v = need_value("--output");
            if (!v) return false;
            out.output_path = v;
        } else if (a == "--format") {
            const char* v = need_value("--format");
            if (!v) return false;
            out.format = v;
        } else if (a == "--runs") {
            const char* v = need_value("--runs");
            if (!v) return false;
            out.cfg.runs = (std::max)(1, std::atoi(v));
        } else if (a == "--warmup") {
            const char* v = need_value("--warmup");
            if (!v) return false;
            out.cfg.warmup = (std::max)(0, std::atoi(v));
        } else if (a == "--samples") {
            const char* v = need_value("--samples");
            if (!v) return false;
            out.cfg.samples = static_cast<size_t>((std::max)(1, std::atoi(v)));
        } else if (a == "--filter") {
            const char* v = need_value("--filter");
            if (!v) return false;
            out.filter = v;
        } else {
            std::cerr << "unknown argument: " << a << "\n";
            return false;
        }
    }
    return true;
}

// Pick the round with most stable percentile: min p99 as final result, filters jitter.
// 选 percentile 最稳定那一轮: p99 最小者作为最终结果, 屏蔽偶发抖动。
BenchResult pick_best(const std::vector<BenchResult>& runs) {
    if (runs.empty()) return {};
    size_t best = 0;
    for (size_t i = 1; i < runs.size(); ++i) {
        if (runs[i].is_throughput) {
            // Throughput: take max (吞吐取最大值)
            if (runs[i].throughput_ops_sec > runs[best].throughput_ops_sec) best = i;
        } else {
            if (runs[i].latency.p99_us < runs[best].latency.p99_us) best = i;
        }
    }
    return runs[best];
}

bool write_output(const std::string& content, const std::string& path) {
    if (path.empty()) {
        std::cout << content;
        return true;
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "failed to open output: " << path << "\n";
        return false;
    }
    f << content;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    CliArgs args;
    if (!parse_args(argc, argv, args)) {
        print_help();
        return 2;
    }
    if (args.show_help) {
        print_help();
        return 0;
    }

    const auto& cases = BenchRegistry::instance().cases();

    if (args.list_only) {
        for (const auto& c : cases) std::cout << c.name << "\n";
        return 0;
    }

    std::regex filter_re;
    bool has_filter = !args.filter.empty();
    if (has_filter) {
        try {
            filter_re = std::regex(args.filter);
        } catch (const std::regex_error& e) {
            std::cerr << "invalid --filter regex: " << e.what() << "\n";
            return 2;
        }
    }

    std::vector<BenchResult> final_results;
    final_results.reserve(cases.size());

    for (const auto& c : cases) {
        if (has_filter && !std::regex_search(c.name, filter_re)) continue;

        std::printf("[bench] %s ...\n", c.name.c_str());

        // Warmup rounds: discard results (预热轮: 丢弃结果)
        for (int w = 0; w < args.cfg.warmup; ++w) {
            (void)c.fn(args.cfg);
        }

        std::vector<BenchResult> rounds;
        rounds.reserve(args.cfg.runs);
        for (int r = 0; r < args.cfg.runs; ++r) {
            BenchResult br = c.fn(args.cfg);
            br.name = c.name;
            rounds.push_back(std::move(br));
        }

        BenchResult best = pick_best(rounds);
        best.name = c.name;
        if (!best.is_throughput) {
            std::printf("       p50=%.2fus p99=%.2fus p999=%.2fus max=%.2fus avg=%.2fus n=%zu\n",
                        best.latency.p50_us, best.latency.p99_us,
                        best.latency.p999_us, best.latency.max_us,
                        best.latency.avg_us, best.n);
        } else {
            std::printf("       %.3e ops/sec  n=%zu\n",
                        best.throughput_ops_sec, best.n);
        }
        final_results.push_back(std::move(best));
    }

    if (final_results.empty()) {
        std::cerr << "no scenarios matched filter\n";
        return 1;
    }

    const std::string machine = collect_machine_info();
    std::string content;
    if (args.format == "md") {
        content = emit_markdown(final_results, machine);
    } else if (args.format == "csv") {
        content = emit_csv(final_results);
    } else if (args.format == "json") {
        content = emit_json(final_results, machine);
    } else {
        std::cerr << "unsupported --format: " << args.format << "\n";
        return 2;
    }

    if (!write_output(content, args.output_path)) return 1;
    if (!args.output_path.empty()) {
        std::printf("[bench] wrote %s (%zu scenarios)\n",
                    args.output_path.c_str(), final_results.size());
    }
    return 0;
}
