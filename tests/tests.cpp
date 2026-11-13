// Testes sem biblioteca externa: cada CHECK que falha imprime a linha e o programa sai com código 1.
// Cada teste usa uma pasta nova dentro da pasta temporária do sistema.

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "../src/crc32.hpp"
#include "../src/store.hpp"

using namespace kvlog;
namespace fs = std::filesystem;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::cerr << __FILE__ << ":" << __LINE__ << "  falhou: " #cond "\n"; \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                                                      \
    do {                                                                                                    \
        ++checks;                                                                                           \
        auto va = (a);                                                                                      \
        auto vb = (b);                                                                                      \
        if (!(va == vb)) {                                                                                  \
            ++failures;                                                                                     \
            std::cerr << __FILE__ << ":" << __LINE__ << "  " #a " == " #b "  (" << va << " != " << vb << ")\n"; \
        }                                                                                                   \
    } while (0)

template <typename E, typename F>
static bool throws(F f) {
    try {
        f();
    } catch (const E&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

static fs::path fresh_dir(const std::string& name) {
    static const auto run = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = fs::temp_directory_path() / ("kvlog_teste_" + std::to_string(run)) / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

static std::string value_or(const std::optional<std::string>& v, const std::string& fallback = "<nada>") {
    return v ? *v : fallback;
}

static std::uint64_t record_size(const std::string& key, const std::string& value) {
    return kRecordHeader + key.size() + value.size();
}

static fs::path log_file(const fs::path& dir, int id) {
    char name[16];
    std::snprintf(name, sizeof name, "%06d.log", id);
    return dir / name;
}

static void flip_byte(const fs::path& file, std::uint64_t offset) {
    std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(static_cast<std::streamoff>(offset));
    char c = 0;
    f.get(c);
    f.seekp(static_cast<std::streamoff>(offset));
    f.put(static_cast<char>(c ^ 0x5A));
}

static Options small_segments() {
    Options o;
    o.segment_bytes = 256;
    o.sync = Sync::Never;
    return o;
}

static void test_crc32() {
    const std::string s = "123456789";
    CHECK_EQ(crc32(s.data(), s.size()), 0xCBF43926u);
    // em pedaços dá o mesmo
    CHECK_EQ(crc32(s.data() + 4, 5, crc32(s.data(), 4)), 0xCBF43926u);
}

static void test_basic() {
    auto dir = fresh_dir("basico");
    {
        auto db = Store::open(dir);
        db->put("a", "1");
        db->put("b", "2");
        db->put("a", "3");  // sobrescreve
        CHECK_EQ(value_or(db->get("a")), std::string("3"));
        CHECK_EQ(value_or(db->get("b")), std::string("2"));
        CHECK(!db->get("c"));
        CHECK(db->del("b"));
        CHECK(!db->del("b"));
        CHECK(!db->get("b"));
        db->put("vazio", "");
        db->put("binário", std::string("a\0b\xff", 4));
        CHECK_EQ(db->keys().size(), std::size_t(3));
    }
    {
        auto db = Store::open(dir);  // tudo de novo a partir do log
        CHECK_EQ(value_or(db->get("a")), std::string("3"));
        CHECK(!db->get("b"));  // a lápide também é relida
        CHECK_EQ(value_or(db->get("vazio"), "<nada>"), std::string(""));
        CHECK_EQ(value_or(db->get("binário")), std::string("a\0b\xff", 4));
        CHECK_EQ(db->recovery().records, std::size_t(6));
        CHECK_EQ(db->recovery().truncated_bytes, std::uint64_t(0));
    }
}

static void test_invalid_input() {
    auto db = Store::open(fresh_dir("entrada"));
    CHECK(throws<std::invalid_argument>([&] { db->put("", "x"); }));
    CHECK(throws<std::invalid_argument>([&] { db->put(std::string(kMaxKey + 1, 'k'), "x"); }));
    CHECK(throws<std::invalid_argument>([&] { db->del(""); }));
}

static void test_rotation() {
    auto dir = fresh_dir("rotacao");
    {
        auto db = Store::open(dir, small_segments());
        for (int i = 0; i < 100; ++i) db->put("chave" + std::to_string(i), "valor" + std::to_string(i));
        CHECK(db->stats().segments > 10);
    }
    auto db = Store::open(dir, small_segments());
    CHECK_EQ(db->stats().keys, std::size_t(100));
    CHECK_EQ(value_or(db->get("chave57")), std::string("valor57"));
}

static void test_compaction() {
    auto dir = fresh_dir("compactacao");
    {
        auto db = Store::open(dir, small_segments());
        for (int round = 0; round < 10; ++round)
            for (int i = 0; i < 20; ++i) db->put("k" + std::to_string(i), "r" + std::to_string(round));
        for (int i = 0; i < 5; ++i) db->del("k" + std::to_string(i));

        auto before = db->stats();
        auto c = db->compact();
        auto after = db->stats();
        CHECK(c.segments_merged > 1);
        CHECK_EQ(c.bytes_before, before.disk_bytes);
        CHECK_EQ(c.bytes_after, before.live_bytes);  // sobrou só o que vale
        CHECK_EQ(after.disk_bytes, after.live_bytes);
        CHECK_EQ(after.segments, std::size_t(2));  // o compactado e o ativo (vazio)
        CHECK_EQ(value_or(db->get("k7")), std::string("r9"));
        CHECK(!db->get("k3"));

        db->put("k3", "voltou");  // escrever depois de compactar continua funcionando
    }
    auto db = Store::open(dir, small_segments());
    CHECK_EQ(db->stats().keys, std::size_t(16));
    CHECK_EQ(value_or(db->get("k3")), std::string("voltou"));
    CHECK(!db->get("k4"));  // lápide não copiada, mas o valor antigo também sumiu
    CHECK_EQ(value_or(db->get("k19")), std::string("r9"));

    // compactar de novo, sem nada novo, não perde nada
    db->compact();
    db->compact();
    CHECK_EQ(db->stats().keys, std::size_t(16));
    CHECK_EQ(value_or(db->get("k12")), std::string("r9"));
}

// Escrita cortada em qualquer byte do último registro: o open descarta só esse registro.
static void test_torn_write_at_every_byte() {
    auto base = fresh_dir("cortado_base");
    {
        auto db = Store::open(base);
        db->put("primeiro", "um");
        db->put("segundo", "dois");
        db->put("terceiro", "três, o que vai ser cortado");
    }
    const auto last = record_size("terceiro", "três, o que vai ser cortado");
    const auto full = fs::file_size(log_file(base, 1));

    int ok = 0;
    for (std::uint64_t cut = 1; cut < last; ++cut) {
        auto dir = fresh_dir("cortado");
        fs::copy_file(log_file(base, 1), log_file(dir, 1));
        fs::resize_file(log_file(dir, 1), full - cut);
        auto db = Store::open(dir);
        bool good = db->recovery().truncated_bytes == last - cut && value_or(db->get("segundo")) == "dois" &&
                    !db->get("terceiro") && fs::file_size(log_file(dir, 1)) == full - last;
        db->put("terceiro", "regravado");  // e o log continua usável depois
        db.reset();
        db = Store::open(dir);
        good = good && value_or(db->get("terceiro")) == "regravado" && db->recovery().truncated_bytes == 0;
        ok += good;
    }
    CHECK_EQ(ok, static_cast<int>(last - 1));
}

static void test_bit_flip_in_active_tail() {
    auto dir = fresh_dir("bit_ativo");
    {
        auto db = Store::open(dir);
        db->put("a", "1");
        db->put("b", "2");
    }
    flip_byte(log_file(dir, 1), fs::file_size(log_file(dir, 1)) - 1);  // último byte do valor de "b"
    auto db = Store::open(dir);
    CHECK_EQ(value_or(db->get("a")), std::string("1"));
    CHECK(!db->get("b"));
    CHECK_EQ(db->recovery().truncated_bytes, record_size("b", "2"));
}

// Arquivo fechado nunca devia mudar. Se mudou, o open recusa em vez de devolver dado errado.
static void test_bit_flip_in_sealed_segment() {
    auto dir = fresh_dir("bit_fechado");
    {
        auto db = Store::open(dir, small_segments());
        for (int i = 0; i < 50; ++i) db->put("k" + std::to_string(i), std::string(40, 'v'));
    }
    flip_byte(log_file(dir, 1), 20);
    CHECK(throws<CorruptionError>([&] { Store::open(dir, small_segments()); }));
}

static void test_get_detects_corruption() {
    auto dir = fresh_dir("leitura_corrompida");
    auto db = Store::open(dir, small_segments());
    for (int i = 0; i < 50; ++i) db->put("k" + std::to_string(i), std::string(40, 'v'));
    flip_byte(log_file(dir, 1), kRecordHeader + 2 + 5);  // valor do primeiro registro, com o banco aberto
    CHECK(throws<CorruptionError>([&] { db->get("k0"); }));
    CHECK_EQ(value_or(db->get("k49")), std::string(40, 'v'));
}

// Queda logo depois do commit da compactação, com e sem parte dos arquivos antigos já apagada.
static void test_crash_after_compaction_commit(bool delete_some) {
    auto dir = fresh_dir(delete_some ? "queda_apagando" : "queda_commit");
    Options o = small_segments();
    {
        auto db = Store::open(dir, o);
        db->put("morto", "não pode voltar");  // vai pro 000001.log
        for (int i = 0; i < 30; ++i) db->put("k" + std::to_string(i), "v" + std::to_string(i));
        db->del("morto");  // a lápide fica num arquivo bem depois
        for (int i = 0; i < 30; ++i) db->put("k" + std::to_string(i), "novo" + std::to_string(i));

        o.stop_after_commit = true;
        auto crashing = Store::open(dir, o);
        db.reset();
        CHECK(throws<SimulatedCrash>([&] { crashing->compact(); }));
    }
    if (delete_some) {
        // caiu no meio do passo 4: apagou os arquivos mais novos (onde está a lápide), mas não o 000001,
        // onde "morto" ainda tem valor. Se o open só relesse o que sobrou, "morto" voltaria.
        int target = 0;
        for (const auto& e : fs::directory_iterator(dir))
            if (e.path().extension() == ".compact") target = std::stoi(e.path().stem().string());
        CHECK(target > 2);
        int deleted = 0;
        for (int id = target; id >= 2; --id) deleted += fs::remove(log_file(dir, id));
        CHECK(deleted > 0);
        CHECK(fs::exists(log_file(dir, 1)));
    }

    o.stop_after_commit = false;
    auto db = Store::open(dir, o);
    CHECK(db->recovery().finished_compaction);
    CHECK(!db->get("morto"));
    CHECK_EQ(db->stats().keys, std::size_t(30));
    CHECK_EQ(value_or(db->get("k17")), std::string("novo17"));
    CHECK(!fs::exists(log_file(dir, 1)));
    int compact_files = 0;
    for (const auto& e : fs::directory_iterator(dir)) compact_files += e.path().extension() == ".compact";
    CHECK_EQ(compact_files, 0);
}

// .tmp é compactação que caiu antes do commit: é lixo e o open apaga.
static void test_leftover_tmp_is_ignored() {
    auto dir = fresh_dir("tmp_sobrando");
    {
        auto db = Store::open(dir);
        db->put("a", "1");
    }
    std::ofstream(dir / "000001.tmp", std::ios::binary) << "lixo de uma compactação pela metade";
    auto db = Store::open(dir);
    CHECK_EQ(value_or(db->get("a")), std::string("1"));
    CHECK(!fs::exists(dir / "000001.tmp"));
}

int main() {
    test_crc32();
    test_basic();
    test_invalid_input();
    test_rotation();
    test_compaction();
    test_torn_write_at_every_byte();
    test_bit_flip_in_active_tail();
    test_bit_flip_in_sealed_segment();
    test_get_detects_corruption();
    test_crash_after_compaction_commit(false);
    test_crash_after_compaction_commit(true);
    test_leftover_tmp_is_ignored();

    std::error_code ec;
    fs::remove_all(fresh_dir("x").parent_path(), ec);

    std::cout << checks - failures << "/" << checks << " verificações ok\n";
    return failures ? 1 : 0;
}
