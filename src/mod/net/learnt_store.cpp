#include "mod/net/learnt_store.h"

#include <string>
#include <vector>

#include "common/learnt_file.h"
#include "mod/game/offsets.h"
#include "mod/log.h"
#include "mod/net/guest_hits.h"
#include "mod/net/player_hits.h"

namespace cdmp::net {
namespace {

constexpr wchar_t kFileName[] = L"cdmp_learnt.txt";
constexpr DWORD kSaveEveryMs = 30000;

std::wstring path_in(const std::wstring& dir) { return dir + kFileName; }

std::string current_text() {
    LearntCombat c;
    guest_hits_learnt_get(c);
    hits_learnt_get(c);
    return c.empty() ? std::string() : learnt::format(c, game::build_2976::kPeTimestamp);
}

// The whole file goes to a .tmp first, then replaces the old one: a game
// closed halfway through a write never leaves half a file.
bool write_file(const std::wstring& path, const std::string& text) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == text.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace

void learnt_load(const std::wstring& dir) {
    const std::wstring path = path_in(dir);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log::info("Learnt: no {} yet; combat values are learnt as you play", "cdmp_learnt.txt");
        return;
    }
    LARGE_INTEGER size{};
    std::string text;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= static_cast<LONGLONG>(learnt::kMaxFileBytes)) {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &read, nullptr)) read = 0;
        text.resize(read);
    }
    CloseHandle(h);
    const auto c = learnt::parse(text, game::build_2976::kPeTimestamp);
    if (!c) {
        log::warn("Learnt: cdmp_learnt.txt is not usable (another game build, or damaged): ignored and replaced");
        return;
    }
    guest_hits_learnt_put(*c);
    hits_learnt_put(*c);
    log::info("Learnt: loaded {} of our attacks, {} enemy defences, {} enemy attacks on us ({} enemy types), "
              "{} reactions",
              c->attacks.size(), c->defence.size(), c->enemy_hits.size(), c->enemy_types.size(), c->reactions.size());
}

void learnt_save_loop(const std::wstring& dir, HANDLE stop) {
    const std::wstring path = path_in(dir);
    // What is on disk now: nothing to write until it changes.
    std::string saved = current_text();
    bool failed_logged = false;
    for (;;) {
        const bool stopping = WaitForSingleObject(stop, kSaveEveryMs) == WAIT_OBJECT_0;
        const std::string text = current_text();
        if (!text.empty() && text != saved) {
            if (write_file(path, text)) {
                saved = text;
            } else if (!failed_logged) {
                failed_logged = true;
                log::warn("Learnt: could not write cdmp_learnt.txt (error {})", GetLastError());
            }
        }
        if (stopping) return;
    }
}

}  // namespace cdmp::net
