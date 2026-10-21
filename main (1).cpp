#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <sstream>
#include <cfloat>

using namespace geode::prelude;

namespace {
    struct Click {
        float x;
        bool down;
        bool p1;
    };

    struct Pending {
        float targetX;
        bool down;
        bool p1;
    };

    std::vector<Click> g_reference;   // clicks from your last COMPLETED run
    std::vector<Click> g_recording;   // clicks from the current run (dead stretches get removed)
    std::vector<Pending> g_pending;   // early inputs waiting to fire, in order
    float g_origin = 0.f;             // X where the current recording began
    bool g_fresh = true;              // true until the first resetLevel of a level session
    bool g_injecting = false;
    bool g_dying = false;             // already handled this death

    std::string levelKey(PlayLayer* pl) {
        return fmt::format("ref-{}-{}", pl->m_level->m_levelID.value(), std::string(pl->m_level->m_levelName));
    }

    std::string encode(std::vector<Click> const& v) {
        std::ostringstream o;
        for (auto& c : v) o << c.x << ':' << (c.down ? 1 : 0) << ':' << (c.p1 ? 1 : 0) << ';';
        return o.str();
    }

    std::vector<Click> decode(std::string const& s) {
        std::vector<Click> out;
        std::istringstream in(s);
        std::string tok;
        while (std::getline(in, tok, ';')) {
            float x; int d, p;
            if (sscanf(tok.c_str(), "%f:%d:%d", &x, &d, &p) == 3)
                out.push_back({x, d != 0, p != 0});
        }
        return out;
    }

    bool enabled() { return Mod::get()->getSettingValue<bool>("enabled"); }
    float window() { return static_cast<float>(Mod::get()->getSettingValue<double>("window")); }

    // Is this reference click already claimed by a queued input?
    bool claimed(Click const& c) {
        for (auto& p : g_pending)
            if (p.p1 == c.p1 && p.down == c.down && p.targetX == c.x) return true;
        return false;
    }

    // Last queued target for a player (so later inputs never fire before earlier ones).
    bool lastPending(bool p1, float& target) {
        for (auto it = g_pending.rbegin(); it != g_pending.rend(); ++it) {
            if (it->p1 == p1) { target = it->targetX; return true; }
        }
        return false;
    }

    float deathTrim() { return static_cast<float>(Mod::get()->getSettingValue<double>("deathTrim")); }

    // Drop a trailing unreleased press per player (left over after cutting a dead stretch).
    void dropUnpairedPresses() {
        for (bool p1 : {true, false}) {
            for (int i = static_cast<int>(g_recording.size()) - 1; i >= 0; --i) {
                if (g_recording[i].p1 != p1) continue;
                if (g_recording[i].down) g_recording.erase(g_recording.begin() + i);
                break;
            }
        }
    }

    // Save the current recording into the reference for this level.
    // cutoffX = how far the recording is trusted (FLT_MAX when the level was completed).
    // Old reference clicks before the recording began, and past the cutoff, are kept.
    void commitRecording(PlayLayer* pl, float cutoffX) {
        std::vector<Click> merged;
        for (auto& c : g_reference)
            if (c.x < g_origin || c.x >= cutoffX) merged.push_back(c);
        for (auto& c : g_recording)
            if (c.x < cutoffX) merged.push_back(c);
        std::stable_sort(merged.begin(), merged.end(),
            [](Click const& a, Click const& b) { return a.x < b.x; });
        g_reference = merged;
        Mod::get()->setSavedValue<std::string>(levelKey(pl), encode(merged));
    }
}

class $modify(SnapBaseLayer, GJBaseGameLayer) {
    float playerX(bool p1) {
        auto* p = p1 ? m_player1 : m_player2;
        return p ? p->getPositionX() : 0.f;
    }

    void handleButton(bool down, int button, bool p1) {
        auto* pl = PlayLayer::get();
        // Only touch the jump button in a real level, and never our own injected input.
        if (g_injecting || button != 1 || !pl || !enabled()) {
            if (pl && button == 1) g_recording.push_back({playerX(p1), down, p1});
            GJBaseGameLayer::handleButton(down, button, p1);
            return;
        }

        float px = playerX(p1);
        float target = px;
        bool delay = false;

        // Find the nearest matching recorded click (press for press, release for release)
        // that is just ahead of us.
        for (auto& c : g_reference) {
            if (c.down != down || c.p1 != p1) continue;
            if (c.x > px + window()) break; // reference is sorted by x
            float gap = c.x - px;
            if (gap > 0.5f && !claimed(c)) {
                target = c.x;
                delay = true;
                break;
            }
        }

        // Keep ordering: if something is already queued for this player, this input
        // has to wait behind it, even with no matching recorded click.
        float queuedTarget;
        if (lastPending(p1, queuedTarget)) {
            target = std::max(target, queuedTarget);
            delay = true;
        }

        if (delay) {
            g_pending.push_back({target, down, p1});
            return; // swallowed; processCommands fires it at the right spot
        }

        g_recording.push_back({px, down, p1});
        GJBaseGameLayer::handleButton(down, button, p1);
    }

    void processCommands(float dt) {
        if (!g_pending.empty() && PlayLayer::get()) {
            // At most one queued input per player per step, so a press and its
            // release never land on the same physics step.
            for (bool p1 : {true, false}) {
                for (size_t i = 0; i < g_pending.size(); ++i) {
                    if (g_pending[i].p1 != p1) continue;
                    if (playerX(p1) >= g_pending[i].targetX) {
                        Pending p = g_pending[i];
                        g_pending.erase(g_pending.begin() + i);
                        g_recording.push_back({playerX(p1), p.down, p1});
                        g_injecting = true;
                        GJBaseGameLayer::handleButton(p.down, 1, p1);
                        g_injecting = false;
                    }
                    break; // only ever look at the first queued input for this player
                }
            }
        }
        GJBaseGameLayer::processCommands(dt);
    }
};

class $modify(SnapPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel(); // runs on death, restart, and checkpoint respawn
        g_pending.clear();
        g_dying = false;

        float startX = m_player1 ? m_player1->getPositionX() : 0.f;

        if (g_fresh || startX < g_origin) {
            // New session, or starting earlier than before: start the recording over.
            g_recording.clear();
            g_origin = startX;
            g_fresh = false;
        } else {
            // Died: throw away every click from the dead stretch. Anything before the
            // respawn point (practice checkpoints) is kept.
            g_recording.erase(
                std::remove_if(g_recording.begin(), g_recording.end(),
                    [&](Click const& c) { return c.x >= startX; }),
                g_recording.end());
            dropUnpairedPresses();
        }

        g_reference = decode(Mod::get()->getSavedValue<std::string>(levelKey(this), ""));
        std::stable_sort(g_reference.begin(), g_reference.end(),
            [](Click const& a, Click const& b) { return a.x < b.x; });
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // First death of this attempt only. Keep every click up to just before the
        // death, drop the last bit (the click that killed you), and save it so the
        // next attempt already knows everything you got through.
        if (!g_dying && enabled() && m_player1 && !m_player1->m_isDead) {
            g_dying = true;
            float deathX = m_player1->getPositionX();
            float cutoff = deathX - deathTrim();
            g_recording.erase(
                std::remove_if(g_recording.begin(), g_recording.end(),
                    [&](Click const& c) { return c.x >= cutoff; }),
                g_recording.end());
            dropUnpairedPresses();
            commitRecording(this, cutoff);
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        // Finished: trust the whole run.
        commitRecording(this, FLT_MAX);
        PlayLayer::levelComplete();
    }

    void onQuit() {
        g_pending.clear();
        g_recording.clear();
        g_fresh = true;
        PlayLayer::onQuit();
    }
};
