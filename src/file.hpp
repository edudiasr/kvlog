#pragma once

// O mínimo de E/S que o banco precisa, com as diferenças entre Windows e Linux escondidas aqui:
// offset de 64 bits, fsync de arquivo e fsync de pasta.

#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace kvlog::io {

std::FILE* open(const std::filesystem::path& path, const char* mode);
void seek(std::FILE* f, std::uint64_t offset);
std::uint64_t size_of(std::FILE* f);
void write_all(std::FILE* f, const void* data, std::size_t n);
bool read_exact(std::FILE* f, void* data, std::size_t n);

// fflush: o dado sai do processo e sobrevive a um kill -9
void flush(std::FILE* f);
// fflush + fsync: o dado chega no disco e sobrevive a uma queda de energia
void sync(std::FILE* f);
// no Linux, criar/renomear/apagar arquivo só é durável depois do fsync da pasta
void sync_dir(const std::filesystem::path& dir);

}  // namespace kvlog::io
