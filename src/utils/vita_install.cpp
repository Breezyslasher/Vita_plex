/*
    VitaPlex — PS Vita in-app self-update

    Installs a downloaded VPK over the running app, the way VitaShell's own
    self-updater does it: extract the ZIP to a scratch directory (validating
    every entry before anything is touched), then overwrite the installed
    files at ux0:app/<TITLE_ID>/ directly and let the caller restart the app.

    The first version of this file promoted the extracted package through
    ScePromoterUtility (the VitaShell/pleNx install path for OTHER apps).
    That can never work for self-update: promoting a title that is currently
    running fails with 0x80101114 — the promoter refuses to replace the
    running app. Overwriting ux0:app/<id> in place is the established
    homebrew answer (HENkaku's unsafe-homebrew mode grants the IO access;
    the running eboot executes from RAM, so replacing its file is safe).
*/

#ifdef __PSV__

#include "utils/vita_install.hpp"

#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>

#include <borealis/core/logger.hpp>
#include <fmt/format.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// miniz is compiled with MINIZ_NO_STDIO / MINIZ_NO_TIME on Vita: all file I/O
// goes through sceIo via custom read/write callbacks, so we never depend on the
// newlib stdio quirks (fopen64/stat64/utime.h).
#include "miniz.h"

namespace {

// ---------------------------------------------------------------------------
// sceIo helpers
// ---------------------------------------------------------------------------

bool fileExists(const std::string& path) {
    SceIoStat st;
    return sceIoGetstat(path.c_str(), &st) >= 0;
}

// mkdir -p, tolerating already-existing components and the device root
// ("ux0:"), which cannot be created.
void mkdirs(const std::string& path) {
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string cur = path.substr(0, j);
        if (!cur.empty() && cur.back() != ':') sceIoMkdir(cur.c_str(), 0777);
        i = j + 1;
    }
}

std::string parentDir(const std::string& path) {
    size_t p = path.find_last_of('/');
    return p == std::string::npos ? std::string() : path.substr(0, p);
}

// Recursively delete a file or directory tree (best effort).
void removePath(const std::string& path) {
    SceUID d = sceIoDopen(path.c_str());
    if (d >= 0) {
        SceIoDirent ent;
        std::memset(&ent, 0, sizeof(ent));
        while (sceIoDread(d, &ent) > 0) {
            std::string name = ent.d_name;
            if (name != "." && name != "..") {
                std::string child = path + "/" + name;
                if (SCE_S_ISDIR(ent.d_stat.st_mode))
                    removePath(child);
                else
                    sceIoRemove(child.c_str());
            }
            std::memset(&ent, 0, sizeof(ent));
        }
        sceIoDclose(d);
        sceIoRmdir(path.c_str());
    } else {
        sceIoRemove(path.c_str());
    }
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    out.resize(size > 0 ? static_cast<size_t>(size) : 0);
    int r = size > 0 ? sceIoRead(fd, out.data(), static_cast<SceSize>(size)) : 0;
    sceIoClose(fd);
    return r == static_cast<int>(size);
}

// Buffered sceIo file copy, overwriting the destination.
bool copyFile(const std::string& src, const std::string& dst) {
    SceUID in = sceIoOpen(src.c_str(), SCE_O_RDONLY, 0);
    if (in < 0) return false;
    SceUID out = sceIoOpen(dst.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (out < 0) { sceIoClose(in); return false; }

    static std::vector<uint8_t> buf(64 * 1024);
    bool ok = true;
    for (;;) {
        int r = sceIoRead(in, buf.data(), static_cast<SceSize>(buf.size()));
        if (r < 0) { ok = false; break; }
        if (r == 0) break;
        const uint8_t* p = buf.data();
        int left = r;
        while (left > 0) {
            int w = sceIoWrite(out, p, static_cast<SceSize>(left));
            if (w <= 0) { ok = false; break; }
            p += w;
            left -= w;
        }
        if (!ok) break;
    }
    sceIoClose(in);
    sceIoClose(out);
    return ok;
}

// Recursively copy src/* over dst/*, creating directories as needed and
// overwriting files in place.
bool copyTree(const std::string& src, const std::string& dst, std::string& err) {
    SceUID d = sceIoDopen(src.c_str());
    if (d < 0) {
        err = "cannot open " + src;
        return false;
    }
    mkdirs(dst);
    SceIoDirent ent;
    std::memset(&ent, 0, sizeof(ent));
    bool ok = true;
    while (ok && sceIoDread(d, &ent) > 0) {
        std::string name = ent.d_name;
        if (name != "." && name != "..") {
            std::string from = src + "/" + name;
            std::string to   = dst + "/" + name;
            if (SCE_S_ISDIR(ent.d_stat.st_mode)) {
                ok = copyTree(from, to, err);
            } else if (!copyFile(from, to)) {
                err = "cannot write " + to;
                ok = false;
            }
        }
        std::memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(d);
    return ok;
}

// ---------------------------------------------------------------------------
// param.sfo string lookup (little-endian format)
// ---------------------------------------------------------------------------

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

bool sfoGetString(const std::vector<uint8_t>& sfo, const char* key, char* out, size_t outsize) {
    if (sfo.size() < 0x14 || le32(&sfo[0]) != 0x46535000 /* "\0PSF" */) return false;
    uint32_t keyTable = le32(&sfo[0x08]);
    uint32_t dataTable = le32(&sfo[0x0C]);
    uint32_t count = le32(&sfo[0x10]);
    for (uint32_t i = 0; i < count; i++) {
        size_t e = 0x14 + static_cast<size_t>(i) * 0x10;
        if (e + 0x10 > sfo.size()) return false;
        uint16_t koff = le16(&sfo[e]);
        uint32_t plen = le32(&sfo[e + 4]);
        uint32_t doff = le32(&sfo[e + 12]);
        size_t kpos = keyTable + koff;
        if (kpos >= sfo.size()) continue;
        if (std::strcmp(reinterpret_cast<const char*>(&sfo[kpos]), key) != 0) continue;
        size_t dpos = dataTable + doff;
        if (dpos >= sfo.size() || outsize == 0) return false;
        size_t n = plen;
        if (n >= outsize) n = outsize - 1;
        if (dpos + n > sfo.size()) n = sfo.size() - dpos;
        std::memcpy(out, &sfo[dpos], n);
        out[n] = '\0';
        return true;
    }
    return false;
}

// The TITLE_ID of the running app, read from its own mounted param.sfo —
// this is what names the install directory under ux0:app/.
std::string runningTitleId() {
    std::vector<uint8_t> sfo;
    if (!readFile("app0:sce_sys/param.sfo", sfo)) return {};
    char titleid[12] = {0};
    if (!sfoGetString(sfo, "TITLE_ID", titleid, sizeof(titleid))) return {};
    return titleid;
}

// ---------------------------------------------------------------------------
// VPK (ZIP) extraction via miniz + sceIo callbacks
// ---------------------------------------------------------------------------

struct ZipReadIO {
    SceUID fd;
};

size_t zipReadCb(void* opaque, mz_uint64 ofs, void* buf, size_t n) {
    SceUID fd = static_cast<ZipReadIO*>(opaque)->fd;
    if (sceIoLseek(fd, static_cast<SceOff>(ofs), SCE_SEEK_SET) < 0) return 0;
    int r = sceIoRead(fd, buf, static_cast<SceSize>(n));
    return r < 0 ? 0 : static_cast<size_t>(r);
}

struct ZipWriteIO {
    SceUID fd;
    bool ok;
};

size_t zipWriteCb(void* opaque, mz_uint64 /*ofs*/, const void* buf, size_t n) {
    ZipWriteIO* w = static_cast<ZipWriteIO*>(opaque);
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t left = n;
    while (left) {
        int r = sceIoWrite(w->fd, p, static_cast<SceSize>(left));
        if (r <= 0) {
            w->ok = false;
            return n - left;
        }
        p += r;
        left -= static_cast<size_t>(r);
    }
    return n;
}

int extractVpk(const std::string& vpk, const std::string& dest, std::string& err,
               const std::function<void(int, int)>& onProgress) {
    SceUID fd = sceIoOpen(vpk.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) {
        err = "cannot open the downloaded VPK";
        return -1;
    }
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);

    ZipReadIO io{fd};
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    zip.m_pRead = zipReadCb;
    zip.m_pIO_opaque = &io;

    if (!mz_zip_reader_init(&zip, static_cast<mz_uint64>(size), 0)) {
        sceIoClose(fd);
        err = "unreadable VPK (invalid ZIP archive)";
        return -1;
    }

    int result = 0;
    mz_uint total = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < total; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            err = "cannot read a VPK entry";
            result = -1;
            break;
        }
        std::string rel = st.m_filename;
        // reject path traversal / absolute entries
        if (rel.empty() || rel[0] == '/' || rel[0] == '\\' || rel.find("..") != std::string::npos)
            continue;
        std::string outpath = dest + "/" + rel;

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            mkdirs(outpath);
            continue;
        }

        mkdirs(parentDir(outpath));
        SceUID out = sceIoOpen(outpath.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (out < 0) {
            err = fmt::format("cannot write {}", rel);
            result = -1;
            break;
        }
        ZipWriteIO w{out, true};
        mz_bool ok = mz_zip_reader_extract_to_callback(&zip, i, zipWriteCb, &w, 0);
        sceIoClose(out);
        if (!ok || !w.ok) {
            err = fmt::format("cannot extract {}", rel);
            result = -1;
            break;
        }
        if (onProgress) onProgress((int)i + 1, (int)total);
    }

    mz_zip_reader_end(&zip);
    sceIoClose(fd);
    return result;
}

}  // namespace

namespace vita {

int installVpk(const std::string& vpkPath, const std::string& workDir, std::string& err,
               std::function<void(int done, int total)> onProgress) {
    brls::Logger::info("vita: installing {} via {}", vpkPath, workDir);

    removePath(workDir);
    mkdirs(workDir);

    // Extract fully to scratch first: every entry is decompressed and CRC
    // checked by miniz before a single installed file is touched, so a
    // corrupt download can never leave the app half-replaced.
    if (extractVpk(vpkPath, workDir, err, onProgress) != 0) {
        brls::Logger::error("vita: extract failed: {}", err);
        removePath(workDir);
        return -1;
    }

    // A valid VPK must at least carry the executable and its metadata.
    if (!fileExists(workDir + "/eboot.bin") || !fileExists(workDir + "/sce_sys/param.sfo")) {
        err = "invalid VPK (eboot.bin or param.sfo missing)";
        brls::Logger::error("vita: {}", err);
        removePath(workDir);
        return -1;
    }

    const std::string titleId = runningTitleId();
    if (titleId.empty()) {
        err = "cannot read the running app's TITLE_ID";
        brls::Logger::error("vita: {}", err);
        removePath(workDir);
        return -1;
    }

    const std::string appDir = "ux0:app/" + titleId;
    if (!fileExists(appDir + "/eboot.bin")) {
        // Writing needs HENkaku's unsafe-homebrew mode, and the install must
        // live on ux0 (every VitaShell install does).
        err = "installed app not found at " + appDir;
        brls::Logger::error("vita: {}", err);
        removePath(workDir);
        return -1;
    }

    std::string copyErr;
    if (!copyTree(workDir, appDir, copyErr)) {
        err = copyErr + " (is HENkaku's unsafe homebrew enabled?)";
        brls::Logger::error("vita: overwrite failed: {}", err);
        removePath(workDir);
        return -1;
    }

    removePath(workDir);
    brls::Logger::info("vita: install succeeded ({} updated in place)", appDir);
    return 0;
}

}  // namespace vita

#endif  // __PSV__
