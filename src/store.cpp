#include "store.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "crc32.hpp"
#include "file.hpp"

namespace fs = std::filesystem;

namespace kvlog {

namespace {

void put_u32(unsigned char* p, std::uint32_t v) {
    p[0] = static_cast<unsigned char>(v);
    p[1] = static_cast<unsigned char>(v >> 8);
    p[2] = static_cast<unsigned char>(v >> 16);
    p[3] = static_cast<unsigned char>(v >> 24);
}

std::uint32_t get_u32(const unsigned char* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}

// value == nullptr monta uma lápide
std::string encode(const std::string& key, const std::string* value) {
    const auto klen = static_cast<std::uint32_t>(key.size());
    const auto vlen = value ? static_cast<std::uint32_t>(value->size()) : kTombstone;
    std::string rec(kRecordHeader, '\0');
    rec.reserve(kRecordHeader + key.size() + (value ? value->size() : 0));
    rec += key;
    if (value) rec += *value;
    // ponteiro pego só depois dos +=, que podem realocar a string
    auto* h = reinterpret_cast<unsigned char*>(&rec[0]);
    put_u32(h + 4, klen);
    put_u32(h + 8, vlen);
    put_u32(h, crc32(h + 4, rec.size() - 4));
    return rec;
}

struct Record {
    std::string key;
    std::string value;
    bool tombstone = false;
    std::uint32_t size = 0;
};

enum class Parse { Ok, End, Torn, Bad };

// Lê o registro que começa na posição atual do arquivo. `remaining` é quanto falta até o fim.
// Torn: o arquivo acaba no meio do registro. Bad: tamanhos absurdos ou CRC errado.
Parse next_record(std::FILE* f, std::uint64_t remaining, Record& r) {
    if (remaining == 0) return Parse::End;
    if (remaining < kRecordHeader) return Parse::Torn;
    unsigned char h[kRecordHeader];
    if (!io::read_exact(f, h, kRecordHeader)) return Parse::Torn;
    const std::uint32_t crc = get_u32(h), klen = get_u32(h + 4), vlen = get_u32(h + 8);
    const bool tomb = vlen == kTombstone;
    const std::uint32_t vbytes = tomb ? 0 : vlen;
    if (klen == 0 || klen > kMaxKey || vbytes > kMaxValue) return Parse::Bad;
    const std::uint64_t total = kRecordHeader + std::uint64_t(klen) + vbytes;
    if (total > remaining) return Parse::Torn;
    r.key.resize(klen);
    r.value.resize(vbytes);
    if (!io::read_exact(f, &r.key[0], klen)) return Parse::Torn;
    if (vbytes && !io::read_exact(f, &r.value[0], vbytes)) return Parse::Torn;
    std::uint32_t c = crc32(h + 4, 8);
    c = crc32(r.key.data(), klen, c);
    c = crc32(r.value.data(), vbytes, c);
    if (c != crc) return Parse::Bad;
    r.tombstone = tomb;
    r.size = static_cast<std::uint32_t>(total);
    return Parse::Ok;
}

// "000012.log" -> 12; qualquer outro nome -> 0
std::uint32_t parse_id(const std::string& name, const std::string& ext) {
    if (name.size() != 6 + ext.size() || name.compare(6, ext.size(), ext) != 0) return 0;
    std::uint32_t id = 0;
    for (int i = 0; i < 6; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return 0;
        id = id * 10 + std::uint32_t(name[i] - '0');
    }
    return id;
}

std::vector<std::uint32_t> list_ids(const fs::path& dir, const std::string& ext) {
    std::vector<std::uint32_t> ids;
    for (const auto& entry : fs::directory_iterator(dir))
        if (auto id = parse_id(entry.path().filename().string(), ext)) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    return ids;
}

void check_key(const std::string& key) {
    if (key.empty()) throw std::invalid_argument("chave vazia");
    if (key.size() > kMaxKey) throw std::invalid_argument("chave maior que 64 KiB");
}

}  // namespace

Store::Store(fs::path dir, Options options) : dir_(std::move(dir)), options_(options) {}

Store::~Store() {
    for (auto& [id, seg] : segments_)
        if (seg.file) std::fclose(seg.file);
}

fs::path Store::path(std::uint32_t id, const char* ext) const {
    char name[32];
    std::snprintf(name, sizeof name, "%06u%s", static_cast<unsigned>(id), ext);
    return dir_ / name;
}

std::unique_ptr<Store> Store::open(const fs::path& dir, Options options) {
    fs::create_directories(dir);
    std::unique_ptr<Store> store(new Store(dir, options));
    store->recover();
    return store;
}

void Store::recover() {
    // .tmp: compactação que caiu antes do commit. O que ela copiou ainda está nos arquivos originais.
    for (auto id : list_ids(dir_, ".tmp")) fs::remove(path(id, ".tmp"));
    // .compact: compactação que passou do commit mas não terminou de trocar os arquivos. Termina agora.
    for (auto id : list_ids(dir_, ".compact")) {
        finish_compaction(id);
        recovery_.finished_compaction = true;
    }

    auto ids = list_ids(dir_, ".log");
    if (ids.empty()) {
        std::fclose(io::open(path(1, ".log"), "wb"));
        io::sync_dir(dir_);
        ids.push_back(1);
    }

    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto id = ids[i];
        const bool last = i + 1 == ids.size();
        std::FILE* f = io::open(path(id, ".log"), "r+b");
        std::uint64_t size = io::size_of(f);
        io::seek(f, 0);

        std::uint64_t offset = 0;
        Record r;
        for (;;) {
            const Parse p = next_record(f, size - offset, r);
            if (p == Parse::End) break;
            if (p != Parse::Ok) {
                if (!last) {
                    std::fclose(f);
                    throw CorruptionError("registro inválido em " + path(id, ".log").string() + " na posição " +
                                          std::to_string(offset));
                }
                // Só o arquivo ativo pode ter sido cortado no meio de uma escrita. Tudo a partir do
                // primeiro registro inválido é descartado, e o arquivo volta pro último ponto bom.
                recovery_.truncated_bytes = size - offset;
                std::fclose(f);
                fs::resize_file(path(id, ".log"), offset);
                f = io::open(path(id, ".log"), "r+b");
                io::sync(f);
                size = offset;
                break;
            }
            if (r.tombstone)
                index_del(r.key);
            else
                index_put(r.key, Location{id, offset, r.size});
            offset += r.size;
            ++recovery_.records;
        }
        segments_[id] = Segment{f, size};
    }
    recovery_.segments = segments_.size();
}

void Store::index_put(const std::string& key, Location loc) {
    auto [it, inserted] = index_.try_emplace(key, loc);
    if (!inserted) {
        live_bytes_ -= it->second.size;
        it->second = loc;
    }
    live_bytes_ += loc.size;
}

void Store::index_del(const std::string& key) {
    auto it = index_.find(key);
    if (it == index_.end()) return;
    live_bytes_ -= it->second.size;
    index_.erase(it);
}

Store::Segment& Store::rotate() {
    io::sync(active().file);  // arquivo fechado vai sempre pro disco, qualquer que seja o modo
    const auto next = active_id() + 1;
    std::FILE* f = io::open(path(next, ".log"), "w+b");
    io::sync_dir(dir_);
    return segments_[next] = Segment{f, 0};
}

Store::Location Store::append(const std::string& record) {
    Segment* seg = &active();
    if (seg->size > 0 && seg->size + record.size() > options_.segment_bytes) seg = &rotate();
    io::seek(seg->file, seg->size);
    io::write_all(seg->file, record.data(), record.size());
    if (options_.sync == Sync::Always)
        io::sync(seg->file);
    else
        io::flush(seg->file);
    Location loc{active_id(), seg->size, static_cast<std::uint32_t>(record.size())};
    seg->size += record.size();
    return loc;
}

void Store::put(const std::string& key, const std::string& value) {
    check_key(key);
    if (value.size() > kMaxValue) throw std::invalid_argument("valor maior que 64 MiB");
    index_put(key, append(encode(key, &value)));
}

bool Store::del(const std::string& key) {
    check_key(key);
    if (!index_.count(key)) return false;
    append(encode(key, nullptr));
    index_del(key);
    return true;
}

std::optional<std::string> Store::get(const std::string& key) {
    auto it = index_.find(key);
    if (it == index_.end()) return std::nullopt;
    const Location& loc = it->second;
    Segment& seg = segments_.at(loc.segment);
    io::seek(seg.file, loc.offset);
    Record r;
    if (next_record(seg.file, loc.size, r) != Parse::Ok || r.key != key)
        throw CorruptionError("valor de '" + key + "' corrompido em " + path(loc.segment, ".log").string());
    return std::move(r.value);
}

std::vector<std::string> Store::keys() const {
    std::vector<std::string> out;
    out.reserve(index_.size());
    for (const auto& [key, loc] : index_) out.push_back(key);
    std::sort(out.begin(), out.end());
    return out;
}

Stats Store::stats() const {
    Stats s;
    s.keys = index_.size();
    s.segments = segments_.size();
    for (const auto& [id, seg] : segments_) s.disk_bytes += seg.size;
    s.live_bytes = live_bytes_;
    return s;
}

// Compactação
//
// 1. fecha o arquivo ativo, assim todo dado existente fica em arquivos fechados (1..N)
// 2. copia só os registros que o índice aponta pra N.tmp e faz fsync
// 3. renomeia N.tmp -> N.compact. Esse rename é o commit: antes dele nada mudou
// 4. apaga 1..N.log e renomeia N.compact -> N.log
//
// Se cair entre 3 e 4, o open() vê o .compact e repete o passo 4. Apagar o que já foi apagado
// e renomear de novo dá no mesmo resultado, então dá pra cair em qualquer ponto.
// As lápides não são copiadas: com 1..N indo embora, não sobra valor antigo pra elas esconderem.
CompactionInfo Store::compact() {
    if (active().size > 0) rotate();
    const auto current = active_id();

    std::vector<std::uint32_t> merged;
    for (const auto& [id, seg] : segments_)
        if (id != current) merged.push_back(id);
    if (merged.empty()) return {};
    const auto target = merged.back();

    CompactionInfo info;
    info.segments_merged = merged.size();
    for (auto id : merged) info.bytes_before += segments_[id].size;

    // registros vivos, na ordem em que estão no disco
    std::vector<Location*> live;
    live.reserve(index_.size());
    for (auto& [key, loc] : index_)
        if (loc.segment <= target) live.push_back(&loc);
    std::sort(live.begin(), live.end(), [](const Location* a, const Location* b) {
        return a->segment != b->segment ? a->segment < b->segment : a->offset < b->offset;
    });

    const auto tmp = path(target, ".tmp");
    std::FILE* out = io::open(tmp, "wb");
    std::vector<std::uint64_t> new_offsets;
    new_offsets.reserve(live.size());
    std::uint64_t written = 0;
    std::string buf;
    try {
        for (const Location* loc : live) {
            Segment& seg = segments_.at(loc->segment);
            buf.resize(loc->size);
            io::seek(seg.file, loc->offset);
            auto* p = reinterpret_cast<unsigned char*>(&buf[0]);
            if (!io::read_exact(seg.file, p, buf.size()) || crc32(p + 4, buf.size() - 4) != get_u32(p))
                throw CorruptionError("registro corrompido em " + path(loc->segment, ".log").string() +
                                      " durante a compactação");
            io::write_all(out, buf.data(), buf.size());
            new_offsets.push_back(written);
            written += buf.size();
        }
        io::sync(out);
    } catch (...) {
        std::fclose(out);
        fs::remove(tmp);
        throw;
    }
    std::fclose(out);

    fs::rename(tmp, path(target, ".compact"));
    io::sync_dir(dir_);
    if (options_.stop_after_commit) throw SimulatedCrash("parou depois do commit da compactação");

    for (auto id : merged) {
        std::fclose(segments_[id].file);
        segments_.erase(id);
    }
    finish_compaction(target);
    segments_[target] = Segment{io::open(path(target, ".log"), "r+b"), written};
    for (std::size_t i = 0; i < live.size(); ++i) *live[i] = Location{target, new_offsets[i], live[i]->size};

    info.bytes_after = written;
    return info;
}

void Store::finish_compaction(std::uint32_t target) {
    for (auto id : list_ids(dir_, ".log"))
        if (id <= target) fs::remove(path(id, ".log"));
    fs::rename(path(target, ".compact"), path(target, ".log"));
    io::sync_dir(dir_);
}

}  // namespace kvlog
