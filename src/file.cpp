#include "file.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace kvlog::io {

static std::runtime_error error(const std::string& what) {
    return std::runtime_error(what + ": " + std::strerror(errno));
}

std::FILE* open(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
    std::wstring wmode(mode, mode + std::strlen(mode));
    std::FILE* f = _wfopen(path.c_str(), wmode.c_str());
#else
    std::FILE* f = std::fopen(path.c_str(), mode);
#endif
    if (!f) throw error("não abriu " + path.string());
    return f;
}

void seek(std::FILE* f, std::uint64_t offset) {
#ifdef _WIN32
    int rc = _fseeki64(f, static_cast<__int64>(offset), SEEK_SET);
#else
    int rc = fseeko(f, static_cast<off_t>(offset), SEEK_SET);
#endif
    if (rc != 0) throw error("fseek");
}

std::uint64_t size_of(std::FILE* f) {
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0) throw error("fseek");
    return static_cast<std::uint64_t>(_ftelli64(f));
#else
    if (fseeko(f, 0, SEEK_END) != 0) throw error("fseek");
    return static_cast<std::uint64_t>(ftello(f));
#endif
}

void write_all(std::FILE* f, const void* data, std::size_t n) {
    if (n && std::fwrite(data, 1, n, f) != n) throw error("fwrite");
}

bool read_exact(std::FILE* f, void* data, std::size_t n) {
    return n == 0 || std::fread(data, 1, n, f) == n;
}

void flush(std::FILE* f) {
    if (std::fflush(f) != 0) throw error("fflush");
}

void sync(std::FILE* f) {
    flush(f);
#ifdef _WIN32
    if (_commit(_fileno(f)) != 0) throw error("_commit");
#else
    if (::fsync(fileno(f)) != 0) throw error("fsync");
#endif
}

void sync_dir(const std::filesystem::path& dir) {
#ifdef _WIN32
    (void)dir;  // no NTFS a entrada da pasta vai junto com o arquivo
#else
    int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) throw error("abrir pasta");
    int rc = ::fsync(fd);
    ::close(fd);
    if (rc != 0) throw error("fsync da pasta");
#endif
}

}  // namespace kvlog::io
