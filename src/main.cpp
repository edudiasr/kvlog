// kvlog: linha de comando do banco.
//
//   kvlog dados put jogador:76561190000000001 '{"kills":12}'
//   kvlog dados get jogador:76561190000000001
//   kvlog dados bench 200000

#include <chrono>
#include <cstdio>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "store.hpp"

using namespace kvlog;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static void usage() {
    std::cerr << "uso: kvlog <pasta> <comando> [argumentos]\n"
                 "\n"
                 "  put <chave> <valor>   grava\n"
                 "  get <chave>           lê\n"
                 "  del <chave>           apaga\n"
                 "  keys                  lista as chaves\n"
                 "  stats                 chaves, arquivos, quanto do disco ainda vale\n"
                 "  compact               reescreve os arquivos fechados só com o que vale\n"
                 "  shell                 lê comandos da entrada padrão, um por linha\n"
                 "  bench [n]             mede escrita, leitura, compactação e abertura (pasta vazia)\n"
                 "  fill <n>              grava n chaves com fsync, imprimindo cada confirmação\n"
                 "  verify <n>            confere se as n primeiras confirmações do fill sobreviveram\n";
}

static double seconds_since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

static std::string human(std::uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024 * 1024)
        std::snprintf(buf, sizeof buf, "%.1f MiB", bytes / 1048576.0);
    else if (bytes >= 1024)
        std::snprintf(buf, sizeof buf, "%.1f KiB", bytes / 1024.0);
    else
        std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}

static void print_stats(const Stats& s) {
    const double pct = s.disk_bytes ? 100.0 * s.live_bytes / s.disk_bytes : 100.0;
    std::printf("chaves: %zu\narquivos: %zu\nem disco: %s\nainda vale: %s (%.0f%%)\n", s.keys, s.segments,
                human(s.disk_bytes).c_str(), human(s.live_bytes).c_str(), pct);
}

static void print_recovery(const Store& db) {
    const auto& r = db.recovery();
    if (r.truncated_bytes)
        std::fprintf(stderr, "recuperação: descartados %llu bytes de uma escrita incompleta no fim do log\n",
                     static_cast<unsigned long long>(r.truncated_bytes));
    if (r.finished_compaction) std::fprintf(stderr, "recuperação: terminou uma compactação interrompida\n");
}

// Executa um comando. Devolve o código de saída.
static int run(Store& db, const std::vector<std::string>& a) {
    const std::string& cmd = a[0];
    if (cmd == "put" && a.size() == 3) {
        db.put(a[1], a[2]);
        return 0;
    }
    if (cmd == "get" && a.size() == 2) {
        auto v = db.get(a[1]);
        if (!v) {
            std::fprintf(stderr, "não existe: %s\n", a[1].c_str());
            return 1;
        }
        std::fwrite(v->data(), 1, v->size(), stdout);
        std::fputc('\n', stdout);
        return 0;
    }
    if (cmd == "del" && a.size() == 2) {
        if (!db.del(a[1])) {
            std::fprintf(stderr, "não existe: %s\n", a[1].c_str());
            return 1;
        }
        return 0;
    }
    if (cmd == "keys" && a.size() == 1) {
        for (const auto& k : db.keys()) std::printf("%s\n", k.c_str());
        return 0;
    }
    if (cmd == "stats" && a.size() == 1) {
        print_stats(db.stats());
        return 0;
    }
    if (cmd == "compact" && a.size() == 1) {
        auto started = Clock::now();
        auto c = db.compact();
        std::printf("%zu arquivos, %s -> %s em %.0f ms\n", c.segments_merged, human(c.bytes_before).c_str(),
                    human(c.bytes_after).c_str(), seconds_since(started) * 1000);
        return 0;
    }
    std::fprintf(stderr, "comando inválido: %s\n", cmd.c_str());
    return 2;
}

static int shell(Store& db) {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::vector<std::string> a;
        std::string word;
        if (!(in >> word)) continue;
        a.push_back(word);
        if (word == "put") {  // put <chave> <resto da linha é o valor>
            std::string key, value;
            in >> key;
            std::getline(in >> std::ws, value);
            a.push_back(key);
            a.push_back(value);
        } else {
            while (in >> word) a.push_back(word);
        }
        if (a[0] == "sair" || a[0] == "exit") break;
        try {
            run(db, a);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "erro: %s\n", e.what());
        }
        std::fflush(stdout);
    }
    return 0;
}

static std::string fill_key(long i) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "k%08ld", i);
    return buf;
}

static std::string fill_value(long i) {
    std::string v = "valor-" + std::to_string(i) + "-";
    while (v.size() < 64) v += v;
    return v.substr(0, 64);
}

static int bench(const fs::path& dir, long n) {
    if (fs::exists(dir) && !fs::is_empty(dir)) {
        std::fprintf(stderr, "o bench precisa de uma pasta vazia (ou que não exista)\n");
        return 2;
    }
    std::mt19937 rng(42);
    std::string value(100, 'x');
    auto key = [](long i) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "jogador:%08ld", i);
        return std::string(buf);
    };

    Options fast;
    fast.sync = Sync::Never;
    {
        auto db = Store::open(dir, fast);
        auto t = Clock::now();
        for (long i = 0; i < n; ++i) db->put(key(i), value);
        double s = seconds_since(t);
        std::printf("escrita (só fflush)    %8ld chaves  %7.2f s  %9.0f ops/s\n", n, s, n / s);

        std::uniform_int_distribution<long> pick(0, n - 1);
        t = Clock::now();
        std::size_t bytes = 0;
        for (long i = 0; i < n; ++i) bytes += db->get(key(pick(rng)))->size();
        s = seconds_since(t);
        std::printf("leitura aleatória      %8ld chaves  %7.2f s  %9.0f ops/s\n", n, s, n / s);

        for (long i = 0; i < n; ++i) db->put(key(i), value);  // tudo regravado: metade do disco vira lixo
        auto before = db->stats();
        t = Clock::now();
        auto c = db->compact();
        s = seconds_since(t);
        std::printf("compactação            %s -> %s em %.2f s (%zu arquivos)\n", human(before.disk_bytes).c_str(),
                    human(c.bytes_after).c_str(), s, c.segments_merged);
    }
    {
        auto t = Clock::now();
        auto db = Store::open(dir, fast);
        double s = seconds_since(t);
        std::printf("abrir e reconstruir    %8zu chaves  %7.2f s\n", db->stats().keys, s);
    }
    {
        Options durable;
        durable.sync = Sync::Always;
        auto db = Store::open(dir, durable);
        const long m = 2000;
        auto t = Clock::now();
        for (long i = 0; i < m; ++i) db->put(key(i), value);
        double s = seconds_since(t);
        std::printf("escrita com fsync      %8ld chaves  %7.2f s  %9.0f ops/s\n", m, s, m / s);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }
    const fs::path dir = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);

    try {
        if (args[0] == "bench") return bench(dir, args.size() > 1 ? std::stol(args[1]) : 200000);

        auto db = Store::open(dir);
        print_recovery(*db);

        if (args[0] == "shell") return shell(*db);

        if (args[0] == "fill" && args.size() == 2) {
            const long n = std::stol(args[1]);
            for (long i = 0; i < n; ++i) {
                db->put(fill_key(i), fill_value(i));
                std::printf("%ld\n", i);  // só sai depois do fsync: é a confirmação pro cliente
                std::fflush(stdout);
            }
            return 0;
        }

        if (args[0] == "verify" && args.size() == 2) {
            const long acked = std::stol(args[1]);
            for (long i = 0; i < acked; ++i) {
                auto v = db->get(fill_key(i));
                if (!v || *v != fill_value(i)) {
                    std::fprintf(stderr, "PERDIDO: %s foi confirmado e não está lá\n", fill_key(i).c_str());
                    return 1;
                }
            }
            // o que está no disco tem que ser um prefixo: 0..m-1, sem buraco
            const long m = static_cast<long>(db->stats().keys);
            for (long i = 0; i < m; ++i)
                if (!db->get(fill_key(i))) {
                    std::fprintf(stderr, "BURACO: %s faltando\n", fill_key(i).c_str());
                    return 1;
                }
            std::printf("ok: %ld confirmadas, %ld no disco, %llu bytes descartados na recuperação\n", acked, m,
                        static_cast<unsigned long long>(db->recovery().truncated_bytes));
            return 0;
        }

        return run(*db, args);
    } catch (const CorruptionError& e) {
        std::fprintf(stderr, "corrompido: %s\n", e.what());
        return 3;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "erro: %s\n", e.what());
        return 1;
    }
}
