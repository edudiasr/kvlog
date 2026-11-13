#pragma once

// Banco chave-valor em log (no estilo do Bitcask).
//
// Toda escrita vai pro fim de um arquivo; nada é sobrescrito no lugar. Um índice em memória
// guarda, pra cada chave, em que arquivo e posição está o valor mais recente. Ler é uma busca
// no índice e uma leitura no disco. Apagar é gravar uma "lápide". A compactação reescreve os
// arquivos antigos só com o que ainda vale.
//
// Uma instância por pasta, usada por uma thread só.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace kvlog {

// Formato de cada registro no disco (inteiros em little-endian):
//
//   crc32  u32   CRC de tudo que vem depois dele
//   klen   u32   tamanho da chave
//   vlen   u32   tamanho do valor; 0xFFFFFFFF quer dizer lápide (chave apagada)
//   chave  klen bytes
//   valor  vlen bytes
inline constexpr std::size_t kRecordHeader = 12;
inline constexpr std::uint32_t kTombstone = 0xFFFFFFFFu;
inline constexpr std::uint32_t kMaxKey = 64 * 1024;
inline constexpr std::uint32_t kMaxValue = 64u * 1024 * 1024;

enum class Sync {
    Always,  // fsync a cada escrita: sobrevive a queda de energia
    Never,   // só fflush: sobrevive a kill do processo, não a queda de energia
};

struct Options {
    std::uint64_t segment_bytes = 4 * 1024 * 1024;  // tamanho em que o arquivo ativo é fechado
    Sync sync = Sync::Always;
    bool stop_after_commit = false;  // só pra teste: simula queda logo depois do commit da compactação
};

struct RecoveryInfo {
    std::size_t segments = 0;
    std::size_t records = 0;
    std::uint64_t truncated_bytes = 0;  // cauda incompleta descartada do arquivo ativo
    bool finished_compaction = false;   // achou uma compactação interrompida e terminou
};

struct Stats {
    std::size_t keys = 0;
    std::size_t segments = 0;
    std::uint64_t disk_bytes = 0;
    std::uint64_t live_bytes = 0;  // bytes dos registros que ainda valem
};

struct CompactionInfo {
    std::size_t segments_merged = 0;
    std::uint64_t bytes_before = 0;
    std::uint64_t bytes_after = 0;
};

class CorruptionError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

class SimulatedCrash : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Store {
public:
    static std::unique_ptr<Store> open(const std::filesystem::path& dir, Options options = {});
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    void put(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    std::vector<std::string> keys() const;

    CompactionInfo compact();
    Stats stats() const;
    const RecoveryInfo& recovery() const { return recovery_; }

private:
    struct Location {
        std::uint32_t segment;
        std::uint64_t offset;
        std::uint32_t size;
    };
    struct Segment {
        std::FILE* file = nullptr;
        std::uint64_t size = 0;
    };

    Store(std::filesystem::path dir, Options options);
    void recover();
    void finish_compaction(std::uint32_t target);
    Location append(const std::string& record);
    Segment& rotate();
    Segment& active() { return segments_.rbegin()->second; }
    std::uint32_t active_id() const { return segments_.rbegin()->first; }
    void index_put(const std::string& key, Location loc);
    void index_del(const std::string& key);
    std::filesystem::path path(std::uint32_t id, const char* ext) const;

    std::filesystem::path dir_;
    Options options_;
    std::map<std::uint32_t, Segment> segments_;  // ordenado por id; o último é o ativo
    std::unordered_map<std::string, Location> index_;
    std::uint64_t live_bytes_ = 0;
    RecoveryInfo recovery_;
};

}  // namespace kvlog
