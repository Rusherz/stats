#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static volatile std::sig_atomic_t g_running = 1;

void on_sigint(int) {
    g_running = 0;
}

struct CpuSnapshot {
    unsigned long long user = 0;
    unsigned long long nice = 0;
    unsigned long long system = 0;
    unsigned long long idle = 0;
    unsigned long long iowait = 0;
    unsigned long long irq = 0;
    unsigned long long softirq = 0;
    unsigned long long steal = 0;
};

struct MemStats {
    unsigned long long total_kb = 0;
    unsigned long long available_kb = 0;
};

struct GpuStats {
    std::string card;
    std::optional<double> gpu_usage_pct;
    std::optional<unsigned long long> vram_used_bytes;
    std::optional<unsigned long long> vram_total_bytes;
    std::optional<double> temp_c;
};

struct Metric {
    std::string label;
    std::optional<double> pct;
    std::string detail;
};

const char* kReset = "\x1b[0m";
const char* kDim = "\x1b[90m";
const char* kGreen = "\x1b[32m";
const char* kYellow = "\x1b[33m";
const char* kRed = "\x1b[31m";
const char* kCyan = "\x1b[36m";

const char* color_for_pct(double pct) {
    if (pct >= 90.0) return kRed;
    if (pct >= 70.0) return kYellow;
    return kGreen;
}

const char* color_for_temp(double c) {
    if (c >= 85.0) return kRed;
    if (c >= 70.0) return kYellow;
    return kGreen;
}

std::string center_text(const std::string& s, int width) {
    if (static_cast<int>(s.size()) >= width) return s.substr(0, width);
    int left = (width - static_cast<int>(s.size())) / 2;
    int right = width - static_cast<int>(s.size()) - left;
    return std::string(left, ' ') + s + std::string(right, ' ');
}

std::optional<unsigned long long> read_ull_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return std::nullopt;
    unsigned long long v;
    f >> v;
    if (!f) return std::nullopt;
    return v;
}

std::optional<std::string> read_first_line(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return std::nullopt;
    std::string s;
    std::getline(f, s);
    if (!f && s.empty()) return std::nullopt;
    return s;
}

std::optional<double> read_temp_c_file(const fs::path& p) {
    auto raw = read_ull_file(p);
    if (!raw) return std::nullopt;
    return static_cast<double>(*raw) / 1000.0;
}

std::optional<double> read_cpu_temp_c() {
    fs::path hwmon("/sys/class/hwmon");
    if (!fs::exists(hwmon) || !fs::is_directory(hwmon)) return std::nullopt;

    for (const auto& e : fs::directory_iterator(hwmon)) {
        if (!e.is_directory()) continue;
        auto name = read_first_line(e.path() / "name");
        if (!name) continue;
        if (*name != "k10temp" && *name != "zenpower" && *name != "coretemp") continue;

        for (int i = 1; i <= 10; ++i) {
            fs::path label_p = e.path() / ("temp" + std::to_string(i) + "_label");
            fs::path input_p = e.path() / ("temp" + std::to_string(i) + "_input");
            auto label = read_first_line(label_p);
            if (label && (*label == "Tctl" || *label == "Tdie" || *label == "Package id 0")) {
                auto t = read_temp_c_file(input_p);
                if (t) return t;
            }
        }

        auto t1 = read_temp_c_file(e.path() / "temp1_input");
        if (t1) return t1;
    }

    return std::nullopt;
}

std::optional<double> read_gpu_temp_c(const fs::path& device_path) {
    fs::path hwmon_dir = device_path / "hwmon";
    if (!fs::exists(hwmon_dir) || !fs::is_directory(hwmon_dir)) return std::nullopt;

    for (const auto& hw : fs::directory_iterator(hwmon_dir)) {
        if (!hw.is_directory()) continue;

        for (int i = 1; i <= 4; ++i) {
            fs::path label_p = hw.path() / ("temp" + std::to_string(i) + "_label");
            fs::path input_p = hw.path() / ("temp" + std::to_string(i) + "_input");
            auto label = read_first_line(label_p);
            if (label && (*label == "edge" || *label == "junction")) {
                auto t = read_temp_c_file(input_p);
                if (t) return t;
            }
        }

        auto t1 = read_temp_c_file(hw.path() / "temp1_input");
        if (t1) return t1;
    }

    return std::nullopt;
}

std::optional<CpuSnapshot> read_cpu_snapshot() {
    std::ifstream f("/proc/stat");
    if (!f) return std::nullopt;

    std::string label;
    CpuSnapshot s;
    f >> label;
    if (label != "cpu") return std::nullopt;

    f >> s.user >> s.nice >> s.system >> s.idle >> s.iowait >> s.irq >> s.softirq >> s.steal;
    if (!f) return std::nullopt;
    return s;
}

std::optional<double> cpu_usage_pct(const CpuSnapshot& a, const CpuSnapshot& b) {
    unsigned long long idle_a = a.idle + a.iowait;
    unsigned long long idle_b = b.idle + b.iowait;

    unsigned long long non_idle_a = a.user + a.nice + a.system + a.irq + a.softirq + a.steal;
    unsigned long long non_idle_b = b.user + b.nice + b.system + b.irq + b.softirq + b.steal;

    unsigned long long total_a = idle_a + non_idle_a;
    unsigned long long total_b = idle_b + non_idle_b;

    if (total_b <= total_a) return std::nullopt;

    double totald = static_cast<double>(total_b - total_a);
    double idled = static_cast<double>(idle_b - idle_a);
    return (totald - idled) * 100.0 / totald;
}

std::optional<MemStats> read_mem_stats() {
    std::ifstream f("/proc/meminfo");
    if (!f) return std::nullopt;

    MemStats m;
    std::string key;
    unsigned long long val;
    std::string unit;

    while (f >> key >> val >> unit) {
        if (key == "MemTotal:") m.total_kb = val;
        if (key == "MemAvailable:") m.available_kb = val;
    }

    if (m.total_kb == 0) return std::nullopt;
    return m;
}

std::vector<GpuStats> read_amd_gpus() {
    std::vector<GpuStats> out;
    fs::path drm("/sys/class/drm");
    if (!fs::exists(drm) || !fs::is_directory(drm)) return out;

    for (const auto& e : fs::directory_iterator(drm)) {
        if (!e.is_directory()) continue;
        std::string name = e.path().filename().string();
        if (name.rfind("card", 0) != 0) continue;
        if (name.find('-') != std::string::npos) continue;

        fs::path device = e.path() / "device";
        if (!fs::exists(device)) continue;

        auto gpu_busy = read_ull_file(device / "gpu_busy_percent");
        auto vram_used = read_ull_file(device / "mem_info_vram_used");
        auto vram_total = read_ull_file(device / "mem_info_vram_total");
        auto gpu_temp = read_gpu_temp_c(device);

        // Skip non-AMD cards that don't expose AMD metrics.
        if (!gpu_busy && !vram_used && !vram_total) continue;

        GpuStats s;
        s.card = name;
        if (gpu_busy) s.gpu_usage_pct = static_cast<double>(*gpu_busy);
        if (vram_used) s.vram_used_bytes = *vram_used;
        if (vram_total) s.vram_total_bytes = *vram_total;
        if (gpu_temp) s.temp_c = *gpu_temp;
        out.push_back(std::move(s));
    }

    return out;
}

std::string human_bytes(unsigned long long bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    int idx = 0;
    while (v >= 1024.0 && idx < 4) {
        v /= 1024.0;
        idx++;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(idx == 0 ? 0 : 2) << v << units[idx];
    return oss.str();
}

void render_vertical_bars(const std::vector<Metric>& metrics, int height = 12) {
    const int colw = 10;
    const std::string full = "█";
    const std::string empty = "░";
    std::vector<int> fills;
    fills.reserve(metrics.size());
    for (const auto& m : metrics) {
        double pct = m.pct ? std::clamp(*m.pct, 0.0, 100.0) : 0.0;
        fills.push_back(static_cast<int>(pct / 100.0 * height + 0.5));
    }

    std::cout << "┌";
    for (size_t i = 0; i < metrics.size() * colw; ++i) std::cout << "─";
    std::cout << "┐\n";

    for (int row = height; row >= 1; --row) {
        std::cout << "│";
        for (size_t i = 0; i < metrics.size(); ++i) {
            const char* color = metrics[i].pct ? color_for_pct(*metrics[i].pct) : kDim;
            std::cout << std::string((colw - 1) / 2, ' ')
                      << (fills[i] >= row ? color : kDim)
                      << (fills[i] >= row ? full : empty) << kReset
                      << std::string(colw / 2, ' ');
        }
        std::cout << "│\n";
    }

    std::cout << "├";
    for (size_t i = 0; i < metrics.size() * colw; ++i) std::cout << "─";
    std::cout << "┤\n";

    std::cout << "│";
    for (const auto& m : metrics) {
        std::string label = m.label;
        if (label.size() > static_cast<size_t>(colw - 1)) label = label.substr(0, colw - 1);
        std::cout << kCyan << center_text(label, colw) << kReset;
    }
    std::cout << "│\n";

    std::cout << "│";
    for (const auto& m : metrics) {
        if (m.pct) {
            std::ostringstream oss;
            oss << static_cast<int>(*m.pct + 0.5) << "%";
            std::cout << color_for_pct(*m.pct) << center_text(oss.str(), colw) << kReset;
        } else {
            std::cout << kDim << center_text("-", colw) << kReset;
        }
    }
    std::cout << "│\n";
    std::cout << "└";
    for (size_t i = 0; i < metrics.size() * colw; ++i) std::cout << "─";
    std::cout << "┘\n\n";

    auto find_detail = [&](const std::string& key) -> std::optional<std::string> {
        for (const auto& m : metrics) {
            if (m.label == key && !m.detail.empty()) return m.detail;
        }
        return std::nullopt;
    };

    auto cpu_t = find_detail("CPU");
    auto gpu_t = find_detail("GPU");
    auto mem_d = find_detail("MEM");
    auto vrm_d = find_detail("VRM");

    if (cpu_t || gpu_t) {
        std::cout << kCyan << "TEMP" << kReset << " : ";
        if (cpu_t) {
            bool ok = cpu_t->find("unavailable") == std::string::npos;
            std::cout << "CPU ";
            if (ok) std::cout << color_for_temp(std::stod(cpu_t->substr(0, cpu_t->size() - 1)));
            std::cout << *cpu_t << kReset;
        } else {
            std::cout << "CPU n/a";
        }
        std::cout << "   ";
        if (gpu_t) {
            bool ok = gpu_t->find("unavailable") == std::string::npos;
            std::cout << "GPU ";
            if (ok) std::cout << color_for_temp(std::stod(gpu_t->substr(0, gpu_t->size() - 1)));
            std::cout << *gpu_t << kReset;
        } else {
            std::cout << "GPU n/a";
        }
        std::cout << "\n";
    }
    if (mem_d) std::cout << kCyan << "MEM " << kReset << " : " << *mem_d << "\n";
    if (vrm_d) std::cout << kCyan << "VRM " << kReset << " : " << *vrm_d << "\n";
}

void clear_screen() {
    std::cout << "\x1b[2J\x1b[H";
}

int main() {
    std::signal(SIGINT, on_sigint);

    auto prev_cpu = read_cpu_snapshot();
    if (!prev_cpu) {
        std::cerr << "Failed to read /proc/stat\n";
        return 1;
    }

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));

        auto curr_cpu = read_cpu_snapshot();
        auto mem = read_mem_stats();
        auto gpus = read_amd_gpus();
        auto cpu_temp = read_cpu_temp_c();

        clear_screen();

        std::vector<Metric> metrics;

        if (curr_cpu) {
            auto cpu_pct = cpu_usage_pct(*prev_cpu, *curr_cpu);
            std::string cpu_detail;
            if (cpu_temp) {
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(1) << *cpu_temp << "C";
                cpu_detail = oss.str();
            } else {
                cpu_detail = "temp unavailable";
            }
            metrics.push_back({"CPU", cpu_pct, cpu_detail});
            prev_cpu = curr_cpu;
        } else {
            metrics.push_back({"CPU", std::nullopt, "unavailable"});
        }

        if (mem) {
            unsigned long long used_kb = mem->total_kb > mem->available_kb ? mem->total_kb - mem->available_kb : 0;
            double mem_pct = mem->total_kb ? static_cast<double>(used_kb) * 100.0 / mem->total_kb : 0.0;
            metrics.push_back({"MEM", mem_pct, human_bytes(used_kb * 1024ULL) + "/" + human_bytes(mem->total_kb * 1024ULL)});
        } else {
            metrics.push_back({"MEM", std::nullopt, "unavailable"});
        }

        if (!gpus.empty()) {
            const auto& g = gpus.front();
            bool multiple_gpus = gpus.size() > 1;
            std::string gpu_detail;
            if (g.temp_c) {
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(1) << *g.temp_c << "C";
                gpu_detail = oss.str();
            } else {
                gpu_detail = "temp unavailable";
            }
            metrics.push_back({"GPU", g.gpu_usage_pct, gpu_detail});

            if (g.vram_used_bytes && g.vram_total_bytes && *g.vram_total_bytes > 0) {
                double vram_pct = static_cast<double>(*g.vram_used_bytes) * 100.0 / *g.vram_total_bytes;
                std::string prefix = multiple_gpus ? (g.card + " ") : "";
                metrics.push_back({"VRM", vram_pct, prefix + human_bytes(*g.vram_used_bytes) + "/" + human_bytes(*g.vram_total_bytes)});
            } else {
                std::string prefix = multiple_gpus ? (g.card + " ") : "";
                metrics.push_back({"VRM", std::nullopt, prefix + "unavailable"});
            }
        } else {
            metrics.push_back({"GPU", std::nullopt, "no AMD GPU metrics found"});
            metrics.push_back({"VRM", std::nullopt, "no AMD VRAM metrics found"});
        }

        render_vertical_bars(metrics);
        std::cout.flush();
    }

    std::cout << "\n";
    return 0;
}
