// photo_timefix.cpp
// C++17 single-file tool
// - Sort by filesystem mtime
// - Read shot time (EXIF/XMP; optional filename fallback)
// - Fill missing shot time by linear interpolation between nearest anchors
// - Optional filename-override rule when fs times drift too far from filename timestamp
// - Write EXIF shot time if missing + sync filesystem times
//
// Build (Windows + vcpkg integrate):
//   - Set C++ Language Standard: /std:c++17
//   - Install exiv2 via vcpkg, platform match x64
//
// Build (Linux):
//   g++ -std=c++17 photo_timefix.cpp -lexiv2 -o photo_timefix

#include <exiv2/exiv2.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#endif

namespace fs = std::filesystem;

struct Options {
    bool recursive = true;
    bool dryRun = true;

    bool enableFilenameFallbackForShot = true;   // when EXIF missing, allow filename to provide shot time (anchor)
    bool enableFilenameOverrideForTarget = true; // if fs times too far from filename timestamp, use filename timestamp as target

    long long filenameOverrideDays = 7;          // "差别过大" 阈值（天）
    long long anchorGapLimitDays = 90;           // 两锚点差太大，不做插值（天）

    bool oneSideStep = true;                     // 只有单侧锚点时，为避免同秒重复，按1秒递增/递减
    long long oneSideStepSeconds = 1;

    bool writeExifIfMissing = true;              // 只对缺失拍摄时间的文件写 EXIF
    bool syncFileTimes = true;                   // 同步文件系统时间到目标时间（Windows含创建时间）

    bool verbose = true;
};

enum class ShotSource {
    None,
    ExifOrXmp,
    Filename
};

struct Item {
    fs::path path;
    std::time_t mtime{};
#ifdef _WIN32
    std::optional<std::time_t> ctime{};
    std::optional<std::time_t> wtime{};
#endif

    std::optional<std::time_t> shot;      // 读取到的“拍摄时间” (anchor)
    ShotSource shotSource = ShotSource::None;

    std::optional<std::time_t> target;    // 最终要写入的时间（EXIF/文件时间）
    std::string targetReason;
};

// ---------- string utils ----------
static std::string trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    return s;
}

static bool hasImageExt(const fs::path& p) {
    if (!p.has_extension()) return false;
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".jpg" || ext == ".jpeg" || ext == ".tif" || ext == ".tiff" || ext == ".png" || ext == ".heic" || ext == ".webp" || ext == ".dng" || ext == ".bmp" || ext == ".gif";
}
static bool hasVideoExt(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });

    return ext == ".mp4" || ext == ".mov" || ext == ".m4v" ||
        ext == ".3gp" || ext == ".3g2" || ext == ".avi" ||
        ext == ".mkv" || ext == ".wmv";
}

static bool hasMediaExt(const fs::path& p) {
    return hasImageExt(p) || hasVideoExt(p);
}


// ---------- time helpers ----------
static bool plausible(std::time_t t) {
    std::time_t now = std::time(nullptr);

    std::tm tm1980{};
    tm1980.tm_year = 80; tm1980.tm_mon = 0; tm1980.tm_mday = 1;
    std::time_t t1980 = std::mktime(&tm1980);

    return t != (std::time_t)-1 && t >= t1980 && t <= now + 24 * 3600; // allow 1 day future
}

static std::string formatLocalTime(std::time_t t) {
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    std::ostringstream oss;
    oss << std::put_time(&lt, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

static long long absDiffSec(std::time_t a, std::time_t b) {
    long long da = (long long)a;
    long long db = (long long)b;
    return (long long)std::llabs(da - db);
}

static std::string normalizeDate(std::string s) {
    s = trim(std::move(s));
    // ISO: 'T' -> ' '
    for (char& c : s) if (c == 'T') c = ' ';
    // YYYY-MM-DD -> YYYY:MM:DD (EXIF style)
    if (s.size() >= 10 && s[4] == '-' && s[7] == '-') { s[4] = ':'; s[7] = ':'; }

    // remove milliseconds
    auto dot = s.find('.');
    if (dot != std::string::npos && dot >= 17) s = s.substr(0, 19);

    // remove timezone suffix if right after seconds
    if (s.size() >= 20) {
        char c = s[19];
        if (c == 'Z' || c == '+' || c == '-') s = s.substr(0, 19);
    }
    return s;
}

static std::optional<std::tm> parseDateTimeToTm(const std::string& raw) {
    std::string s = normalizeDate(raw);
    if (s.size() < 19) return std::nullopt;
    if (s.rfind("0000:00:00", 0) == 0) return std::nullopt;

    std::tm tm{};
    std::istringstream iss(s.substr(0, 19));
    iss >> std::get_time(&tm, "%Y:%m:%d %H:%M:%S");
    if (iss.fail()) return std::nullopt;
    tm.tm_isdst = -1;
    return tm;
}

static std::optional<std::time_t> tmToTimeTLocal(std::tm tm) {
    std::time_t t = std::mktime(&tm); // local time
    if (t == (std::time_t)-1) return std::nullopt;
    return t;
}

static std::string toExifString(std::time_t t) {
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    std::ostringstream oss;
    oss << std::put_time(&lt, "%Y:%m:%d %H:%M:%S");
    return oss.str();
}

// ---------- UTF-8 <-> Wide (Windows) ----------
#ifdef _WIN32
static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len);
    return out;
}

static std::string wideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), out.data(), len, nullptr, nullptr);
    return out;
}

static std::string pathToUtf8(const fs::path& p) {
    return wideToUtf8(p.wstring());
}
#else
static std::string pathToUtf8(const fs::path& p) {
    // On non-Windows, filesystem path is typically UTF-8 already
    return p.u8string();
}
#endif

// ---------- filename timestamp parsing ----------
static std::optional<std::time_t> parseFilenameTime(const fs::path& file) {
    // Support patterns like:
    // Screenshot_20211230_215425
    // 20211230_215425 / 20211230215425
    // 2021-12-30_21-54-25 etc.
    std::string name = file.stem().string();

    const std::vector<std::regex> patterns = {
        std::regex(R"((\d{4})(\d{2})(\d{2})[_-]?(\d{2})(\d{2})(\d{2}))"),
        std::regex(R"((\d{4})-(\d{2})-(\d{2})[_\s-]?(\d{2})[-_]?(\d{2})[-_]?(\d{2}))")
    };

    for (const auto& re : patterns) {
        std::smatch m;
        if (std::regex_search(name, m, re)) {
            int Y = std::stoi(m[1].str());
            int M = std::stoi(m[2].str());
            int D = std::stoi(m[3].str());
            int h = std::stoi(m[4].str());
            int mi = std::stoi(m[5].str());
            int s = std::stoi(m[6].str());

            std::tm tm{};
            tm.tm_year = Y - 1900;
            tm.tm_mon = M - 1;
            tm.tm_mday = D;
            tm.tm_hour = h;
            tm.tm_min = mi;
            tm.tm_sec = s;
            tm.tm_isdst = -1;

            auto tOpt = tmToTimeTLocal(tm);
            if (tOpt && plausible(*tOpt)) return tOpt;
        }
    }
    return std::nullopt;
}

// ---------- Exiv2: read best shot time ----------
static std::optional<std::time_t> readShotTimeFromMetadata(const fs::path& file) {
    try {
        auto image = Exiv2::ImageFactory::open(pathToUtf8(file));
        if (!image.get()) return std::nullopt;

        image->readMetadata();
        auto& exif = image->exifData();
        auto& xmp = image->xmpData();

        auto tryExifKeys = [&](const std::vector<std::string>& keys) -> std::optional<std::time_t> {
            for (const auto& k : keys) {
                auto it = exif.findKey(Exiv2::ExifKey(k));
                if (it == exif.end()) continue;
                auto tmOpt = parseDateTimeToTm(it->toString());
                if (!tmOpt) continue;
                auto tOpt = tmToTimeTLocal(*tmOpt);
                if (!tOpt || !plausible(*tOpt)) continue;
                return tOpt;
            }
            return std::nullopt;
            };

        auto tryXmpKeys = [&](const std::vector<std::string>& keys) -> std::optional<std::time_t> {
            for (const auto& k : keys) {
                auto it = xmp.findKey(Exiv2::XmpKey(k));
                if (it == xmp.end()) continue;
                auto tmOpt = parseDateTimeToTm(it->toString());
                if (!tmOpt) continue;
                auto tOpt = tmToTimeTLocal(*tmOpt);
                if (!tOpt || !plausible(*tOpt)) continue;
                return tOpt;
            }
            return std::nullopt;
            };

        if (!exif.empty()) {
            // priority: DateTimeOriginal > Digitized > Image.DateTime
            if (auto t = tryExifKeys({ "Exif.Photo.DateTimeOriginal" })) return t;
            if (auto t = tryExifKeys({ "Exif.Photo.DateTimeDigitized" })) return t;
            if (auto t = tryExifKeys({ "Exif.Image.DateTime" })) return t;
        }
        if (!xmp.empty()) {
            if (auto t = tryXmpKeys({ "Xmp.exif.DateTimeOriginal" })) return t;
            if (auto t = tryXmpKeys({ "Xmp.xmp.CreateDate" })) return t;
            if (auto t = tryXmpKeys({ "Xmp.photoshop.DateCreated" })) return t;
        }
        return std::nullopt;
    }
    catch (...) {
        return std::nullopt;
    }
}

// ---------- Exiv2: write EXIF shot time only if missing ----------
static bool writeExifShotIfMissing(const fs::path& file, std::time_t t, bool verbose) {
    try {
        auto img = Exiv2::ImageFactory::open(pathToUtf8(file));
        if (!img.get()) return false;

        img->readMetadata();
        auto& exif = img->exifData();

        std::string s = toExifString(t);
        bool changed = false;

        Exiv2::ExifKey k1("Exif.Photo.DateTimeOriginal");
        Exiv2::ExifKey k2("Exif.Photo.DateTimeDigitized");
        Exiv2::ExifKey k3("Exif.Image.DateTime");

        if (exif.findKey(k1) == exif.end()) { exif["Exif.Photo.DateTimeOriginal"] = s; changed = true; }
        if (exif.findKey(k2) == exif.end()) { exif["Exif.Photo.DateTimeDigitized"] = s; changed = true; }
        if (exif.findKey(k3) == exif.end()) { exif["Exif.Image.DateTime"] = s; changed = true; }

        if (changed) {
            img->setExifData(exif);
            img->writeMetadata();
        }
        return changed;
    }
    catch (const std::exception& e) {
        if (verbose) std::cout << "    EXIF write failed: " << e.what() << "\n";
        return false;
    }
    catch (...) {
        if (verbose) std::cout << "    EXIF write failed: unknown error\n";
        return false;
    }
}

// ---------- filesystem times ----------
static std::time_t to_time_t_from_fs_time(fs::file_time_type ftt) {
    using namespace std::chrono;
    auto sctp = time_point_cast<system_clock::duration>(
        ftt - fs::file_time_type::clock::now() + system_clock::now()
    );
    return system_clock::to_time_t(sctp);
}

#ifdef _WIN32
struct WinTimes { std::time_t create{}; std::time_t write{}; };

static std::optional<std::time_t> filetimeToTimeT(const FILETIME& ft) {
    ULARGE_INTEGER ull;
    ull.LowPart = ft.dwLowDateTime;
    ull.HighPart = ft.dwHighDateTime;

    constexpr long long WINDOWS_TICK = 10000000LL;
    constexpr long long SEC_TO_UNIX_EPOCH = 11644473600LL;

    long long secondsSince1601 = (long long)(ull.QuadPart / WINDOWS_TICK);
    long long unixSeconds = secondsSince1601 - SEC_TO_UNIX_EPOCH;
    if (unixSeconds < 0) return std::nullopt;
    return (std::time_t)unixSeconds;
}

static std::optional<WinTimes> getFileTimesWindows(const fs::path& file) {
    HANDLE h = CreateFileW(file.wstring().c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;

    FILETIME c{}, a{}, w{};
    BOOL ok = GetFileTime(h, &c, &a, &w);
    CloseHandle(h);
    if (!ok) return std::nullopt;

    auto tc = filetimeToTimeT(c);
    auto tw = filetimeToTimeT(w);
    if (!tc || !tw) return std::nullopt;

    return WinTimes{ *tc, *tw };
}

static bool setFileTimesWindows(const fs::path& file, std::time_t t, bool verbose) {
    constexpr long long WINDOWS_TICK = 10000000LL;
    constexpr long long SEC_TO_UNIX_EPOCH = 11644473600LL;

    long long ft64 = (static_cast<long long>(t) + SEC_TO_UNIX_EPOCH) * WINDOWS_TICK;

    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(ft64 & 0xFFFFFFFF);
    ft.dwHighDateTime = static_cast<DWORD>((ft64 >> 32) & 0xFFFFFFFF);

    HANDLE h = CreateFileW(file.wstring().c_str(),
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (verbose) std::cout << "    SetFileTime: cannot open (err=" << GetLastError() << ")\n";
        return false;
    }

    BOOL ok = SetFileTime(h, &ft, &ft, &ft); // create/access/write
    if (!ok && verbose) std::cout << "    SetFileTime failed (err=" << GetLastError() << ")\n";
    CloseHandle(h);
    return ok != 0;
}
#else
static bool setFileTimesPosix(const fs::path& file, std::time_t t, bool verbose) {
    timespec ts[2];
    ts[0].tv_sec = t; ts[0].tv_nsec = 0; // atime
    ts[1].tv_sec = t; ts[1].tv_nsec = 0; // mtime
    int ret = utimensat(AT_FDCWD, file.c_str(), ts, 0);
    if (ret != 0 && verbose) std::cout << "    utimensat failed\n";
    return ret == 0;
}
#endif

// ---------- collect files ----------
static void collectFiles(const fs::path& root, bool recursive, std::vector<Item>& out) {
    std::error_code ec;

    if (fs::is_regular_file(root, ec)) {
        if (hasMediaExt(root)) { // 改：图片或视频
            Item it;
            it.path = root;
            it.mtime = to_time_t_from_fs_time(fs::last_write_time(root, ec));
#ifdef _WIN32
            if (auto wt = getFileTimesWindows(root)) {
                it.ctime = wt->create;
                it.wtime = wt->write;
            }
#endif
            out.push_back(std::move(it));
        }
        return;
    }

    if (!fs::is_directory(root, ec)) return; // 修正这里

    if (recursive) {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
            it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (!it->is_regular_file(ec)) continue;
            fs::path p = it->path();
            if (!hasMediaExt(p)) continue; // 改：图片或视频

            Item item;
            item.path = p;
            item.mtime = to_time_t_from_fs_time(fs::last_write_time(p, ec));
#ifdef _WIN32
            if (auto wt = getFileTimesWindows(p)) {
                item.ctime = wt->create;
                item.wtime = wt->write;
            }
#endif
            out.push_back(std::move(item));
        }
    }
    else {
        for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
            it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (!it->is_regular_file(ec)) continue;
            fs::path p = it->path();
            if (!hasMediaExt(p)) continue; // 改：图片或视频

            Item item;
            item.path = p;
            item.mtime = to_time_t_from_fs_time(fs::last_write_time(p, ec));
#ifdef _WIN32
            if (auto wt = getFileTimesWindows(p)) {
                item.ctime = wt->create;
                item.wtime = wt->write;
            }
#endif
            out.push_back(std::move(item));
        }
    }
}


// ---------- choose shot time (anchor) ----------
static void fillShotTime(Item& it, const Options& opt) {
    // 1) Try metadata
    if (auto t = readShotTimeFromMetadata(it.path)) {
        it.shot = t;
        it.shotSource = ShotSource::ExifOrXmp;
        return;
    }

    // 2) Optional filename fallback
    if (opt.enableFilenameFallbackForShot) {
        if (auto nt = parseFilenameTime(it.path)) {
            it.shot = nt;
            it.shotSource = ShotSource::Filename;
            return;
        }
    }

    it.shot = std::nullopt;
    it.shotSource = ShotSource::None;
}

// ---------- filename override rule for target ----------
static void applyFilenameOverrideForTarget(Item& it, const Options& opt) {
    if (!opt.enableFilenameOverrideForTarget) return;

    auto nt = parseFilenameTime(it.path);
    if (!nt) return;

    long long thresholdSec = opt.filenameOverrideDays * 86400LL;

#ifdef _WIN32
    // If both create and write times exist and both are far from filename time, override.
    if (it.ctime && it.wtime) {
        long long dc = absDiffSec(*it.ctime, *nt);
        long long dw = absDiffSec(*it.wtime, *nt);
        if (dc > thresholdSec && dw > thresholdSec) {
            it.target = nt;
            it.targetReason = "filename override (fs create/write too far)";
        }
    }
    else {
        // Fallback: compare mtime only
        long long dm = absDiffSec(it.mtime, *nt);
        if (dm > thresholdSec) {
            it.target = nt;
            it.targetReason = "filename override (mtime too far)";
        }
    }
#else
    long long dm = absDiffSec(it.mtime, *nt);
    if (dm > thresholdSec) {
        it.target = nt;
        it.targetReason = "filename override (mtime too far)";
    }
#endif
}

// ---------- interpolation for missing shot times ----------
static void inferMissingByInterpolation(std::vector<Item>& items, const Options& opt) {
    // items must be sorted by mtime
    long long gapLimitSec = opt.anchorGapLimitDays * 86400LL;

    int n = (int)items.size();
    int i = 0;
    while (i < n) {
        if (items[i].shot) { i++; continue; }

        int L = i;
        while (i < n && !items[i].shot) i++;
        int R = i - 1;
        int m = R - L + 1;

        std::optional<std::time_t> Tprev, Tnext;
        if (L - 1 >= 0) Tprev = items[L - 1].shot;
        if (R + 1 < n)  Tnext = items[R + 1].shot;

        // We only set target here (for missing shot ones), but not overwrite existing target chosen by filename override.
        auto setIfEmptyTarget = [&](int idx, std::time_t t, const std::string& reason) {
            if (!items[idx].target) {
                items[idx].target = t;
                items[idx].targetReason = reason;
            }
            };

        if (Tprev && Tnext) {
            long long gap = (long long)(*Tnext - *Tprev);
            if (std::llabs(gap) > gapLimitSec) {
                // Too large gap: avoid interpolation -> nearest fill by position
                for (int j = 0; j < m; ++j) {
                    std::time_t t = (j < m / 2 ? *Tprev : *Tnext);
                    setIfEmptyTarget(L + j, t, "gap too large -> nearest anchor fill");
                }
            }
            else {
                // Improved interpolation: guarantee distinct timestamps when anchors are too close
                long long absGap = std::llabs(gap);

                // Need at least (m+1) seconds difference to give every missing file a unique second
                // Example: m=5 => need >=6 seconds span between anchors.
                if (absGap < (long long)(m + 1)) {
                    // Force step fill to avoid same-timestamp collapse
                    long long dir = (gap >= 0) ? 1 : -1; // keep monotonic direction consistent with anchors
                    for (int k = 1; k <= m; ++k) {
                        std::time_t t = (std::time_t)((long long)(*Tprev) + dir * k * opt.oneSideStepSeconds);
                        setIfEmptyTarget(L + (k - 1), t, "anchors too close -> step-filled");
                    }
                }
                else {
                    // True linear interpolation (enough span)
                    for (int k = 1; k <= m; ++k) {
                        long long add = gap * k / (m + 1);
                        std::time_t t = (std::time_t)((long long)(*Tprev) + add);
                        setIfEmptyTarget(L + (k - 1), t, "interpolated between anchors");
                    }
                }
            }
        }
        else if (Tprev && !Tnext) {
            for (int j = 0; j < m; ++j) {
                // j 从 0 开始，所以用 (j+1) 才是 prev+1s, prev+2s...
                std::time_t t = opt.oneSideStep ? (*Tprev + (j + 1) * opt.oneSideStepSeconds) : *Tprev;
                setIfEmptyTarget(L + j, t,
                    opt.oneSideStep ? "only prev anchor -> filled +1s steps" : "only prev anchor -> filled");
            }
        }
        else if (!Tprev && Tnext) {
            for (int j = 0; j < m; ++j) {
                // 让最靠近 next 的那张是 next-1s，保证都落在 next 之前
                std::time_t t = opt.oneSideStep ? (*Tnext - (m - j) * opt.oneSideStepSeconds) : *Tnext;
                setIfEmptyTarget(L + j, t,
                    opt.oneSideStep ? "only next anchor -> filled -1s steps" : "only next anchor -> filled");
            }
        }

        else {
            // no anchors at all: leave empty
            for (int j = 0; j < m; ++j) {
                // nothing
            }
        }
    }
}
// ---------- make filled targets unique (+1s/+2s ...) ----------
static void makeFilledTargetsStrictlyIncreasing(std::vector<Item>& items, const Options& opt) {
    // assumes items already sorted by mtime
    const long long step = std::max(1LL, opt.oneSideStepSeconds);

    std::optional<std::time_t> prevTarget;
    for (auto& it : items) {
        if (!it.target) continue;

        // 只修正“需要推断出来的文件”：也就是原本没有 shot 的那批 ([FILL])
        const bool isFilled = !it.shot.has_value();

        if (prevTarget && isFilled && *it.target <= *prevTarget) {
            // bump to prev+step, prev+2step, ...
            std::time_t bumped = (std::time_t)((long long)(*prevTarget) + step);
            it.target = bumped;

            if (!it.targetReason.empty()) it.targetReason += " + ";
            it.targetReason += "unique(+1s steps)";
        }

        // 更新 prevTarget：以“最终 target”为准
        prevTarget = it.target;
    }
}

// ---------- interactive input helpers ----------
static bool askYesNo(const std::string& q, bool def) {
    std::cout << q << (def ? " [Y/n]: " : " [y/N]: ");
    std::string s;
    std::getline(std::cin, s);
    s = trim(s);
    if (s.empty()) return def;
    char c = (char)std::tolower((unsigned char)s[0]);
    return c == 'y' || c == '1' || c == 't';
}

static long long askInt64(const std::string& q, long long def) {
    std::cout << q << " (default " << def << "): ";
    std::string s;
    std::getline(std::cin, s);
    s = trim(s);
    if (s.empty()) return def;
    try { return std::stoll(s); }
    catch (...) { return def; }
}

static fs::path askPath() {
#ifdef _WIN32
    // make console accept UTF-8 in many terminals
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::cout << "Input file or folder path (you can paste / drag-drop):\n> ";
    std::string in;
    std::getline(std::cin, in);
    in = trim(in);

    // strip surrounding quotes
    if (in.size() >= 2 && ((in.front() == '"' && in.back() == '"') || (in.front() == '\'' && in.back() == '\''))) {
        in = in.substr(1, in.size() - 2);
    }

#ifdef _WIN32
    // treat input as UTF-8
    std::wstring w = utf8ToWide(in);
    return fs::path(w);
#else
    return fs::path(in);
#endif
}

// ---------- main ----------
int main(int argc, char** argv) {
    Options opt;

    std::cout << "Photo Time Fix (mtime-sort + EXIF read + interpolate missing)\n";
    std::cout << "Tips: first run with dry-run = yes.\n\n";

    fs::path root = askPath();

    if (root.empty()) {
        std::cout << "No path provided.\n";
        return 1;
    }

    std::error_code ec;
    if (!fs::exists(root, ec)) {
        std::cout << "Path not found.\n";
        return 1;
    }

    // interactive options
    opt.recursive = askYesNo("Recursive scan?", true);
    opt.dryRun = askYesNo("Dry-run (no changes)?", true);

    opt.enableFilenameFallbackForShot = askYesNo("If EXIF missing, allow filename timestamp as shot time (anchor)?", true);
    opt.enableFilenameOverrideForTarget = askYesNo("If fs times drift too far from filename timestamp, override target by filename?", true);

    opt.filenameOverrideDays = askInt64("Filename override threshold days", 7);
    opt.anchorGapLimitDays = askInt64("Anchor gap limit days (too large -> no interpolation)", 90);

    opt.oneSideStep = askYesNo("When only one anchor exists, apply +1s steps to avoid same timestamp?", true);

    opt.writeExifIfMissing = askYesNo("Write EXIF shot time if missing (DateTimeOriginal/Digitized/Image.DateTime)?", true);
    opt.syncFileTimes = askYesNo("Sync filesystem times to target time?", true);

    std::cout << "\nScanning...\n";

    std::vector<Item> items;
    collectFiles(root, opt.recursive, items);

    if (items.empty()) {
        std::cout << "No image files found.\n";
        return 0;
    }

    // fill shot time for each item
    int anchors = 0;
    for (auto& it : items) {
        fillShotTime(it, opt);
        if (it.shot) anchors++;
    }

    // sort by mtime (and path as tie-breaker)
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.mtime != b.mtime) return a.mtime < b.mtime;
        return a.path.u8string() < b.path.u8string();
        });

    // pre-apply filename override for target (this can set target even if shot exists)
    if (opt.enableFilenameOverrideForTarget) {
        for (auto& it : items) applyFilenameOverrideForTarget(it, opt);
    }

    // For files that already have shot time and don't have a target yet, set target = shot
    for (auto& it : items) {
        if (!it.target && it.shot) {
            it.target = it.shot;
            it.targetReason = (it.shotSource == ShotSource::Filename) ? "shot from filename" : "shot from metadata";
        }
    }

    // interpolate only for files that have NO shot (missing) AND target not set by filename override
    inferMissingByInterpolation(items, opt);
    // 新增：把所有 [FILL] 的 target 做 +1s/+2s 去重兜底
    makeFilledTargetsStrictlyIncreasing(items, opt);
    // apply changes
    int changedExif = 0, changedFs = 0;
    int filledCount = 0, skippedNoTarget = 0;
    int processed = 0;

    std::cout << "\nFiles: " << items.size() << ", anchors(with shot): " << anchors << "\n";
    std::cout << "----\n";

    for (auto& it : items) {
        processed++;

        bool missingShot = !it.shot.has_value(); // metadata+filename anchor both missing
        // But in our design, "missingShot" means no shot extracted; still may have target via interpolation/override.

        if (!it.target) {
            skippedNoTarget++;
            if (opt.verbose) {
                std::cout << "[SKIP] " << it.path << " (no target time inferred)\n";
            }
            continue;
        }

        // count filled: originally no shot time AND now has target
        if (!it.shot && it.target) filledCount++;

        std::cout << (it.shot ? "[OK]   " : "[FILL] ")
            << it.path << "\n"
            << "       target: " << formatLocalTime(*it.target)
            << "   (" << it.targetReason << ")\n"
            << "       mtime : " << formatLocalTime(it.mtime) << "\n";

#ifdef _WIN32
        if (it.ctime && it.wtime) {
            std::cout << "       ctime : " << formatLocalTime(*it.ctime) << "\n";
            std::cout << "       wtime : " << formatLocalTime(*it.wtime) << "\n";
        }
#endif

        if (opt.dryRun) {
            std::cout << "       dry-run: no changes\n";
            std::cout << "----\n";
            continue;
        }

        // 1) write EXIF only if missing shot in metadata (safer: only write when metadata had no usable shot)
        // Here "missingShot" might be true even if filename used as shot anchor earlier. We prefer to detect metadata-missing:
        bool metadataHadShot = (it.shotSource == ShotSource::ExifOrXmp);
        bool shouldWriteExif = opt.writeExifIfMissing && !metadataHadShot; // only if metadata didn't already provide shot

        if (shouldWriteExif) {
            if (writeExifShotIfMissing(it.path, *it.target, opt.verbose)) {
                changedExif++;
                std::cout << "       EXIF: written (missing keys)\n";
            }
            else {
                std::cout << "       EXIF: not written (maybe unsupported format or already present)\n";
            }
        }

        // 2) sync file system times
        if (opt.syncFileTimes) {
#ifdef _WIN32
            if (setFileTimesWindows(it.path, *it.target, opt.verbose)) {
                changedFs++;
                std::cout << "       FS  : times updated\n";
            }
            else {
                std::cout << "       FS  : update failed\n";
            }
#else
            if (setFileTimesPosix(it.path, *it.target, opt.verbose)) {
                changedFs++;
                std::cout << "       FS  : times updated\n";
            }
            else {
                std::cout << "       FS  : update failed\n";
            }
#endif
        }

        std::cout << "----\n";
    }

    std::cout << "\nDone.\n";
    std::cout << "Filled missing (no shot -> inferred target): " << filledCount << "\n";
    std::cout << "No-target skipped: " << skippedNoTarget << "\n";
    if (!opt.dryRun) {
        std::cout << "EXIF updated (missing-only): " << changedExif << "\n";
        std::cout << "Filesystem times updated: " << changedFs << "\n";
    }
    else {
        std::cout << "Dry-run mode: no changes made.\n";
    }

    return 0;
}
