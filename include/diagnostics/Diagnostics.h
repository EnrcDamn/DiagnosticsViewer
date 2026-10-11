#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace diagnostics
{
inline constexpr std::size_t maxScopes = 8;
inline constexpr std::size_t maxValues = 8;

struct Config
{
    std::string application = "Application";
    // Scope zero is the whole block. Register names before audio starts.
    std::vector<std::string> scopes { "block" };
    std::vector<std::string> values;
    // Empty selects the system temp directory / Diagnostics.
    std::filesystem::path directory;
    // Optional, non-realtime compatibility feature. See README for ownership rules.
    bool captureStandardStreams = false;
};

struct BlockInfo
{
    double budgetMs = 0.0;
    std::array<double, maxValues> values {};
};

struct ScopeStats
{
    double totalMs = 0.0;
    double selfMs = 0.0;
    std::uint32_t calls = 0;
    // Bit i records that this scope ran directly inside scope i.
    std::uint8_t parents = 0;
};

struct Sample
{
    BlockInfo info;
    std::array<ScopeStats, maxScopes> scopes {};
};

class Block;

// Own on a control thread; destroy after callbacks stop. One callback producer
// per Session. Reports, aggregation and debug output run on its worker thread;
// session setup opens its file on the owning control thread.
class Session
{
public:
    explicit Session(Config config);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Non-realtime logging only. Bounded messages; a busy/full queue drops logs.
    void log(std::string_view message, std::string_view level = "info") noexcept;
    const std::filesystem::path& file() const noexcept;

private:
    friend class Block;
    void submit(const Sample&) noexcept;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Fixed storage, no allocations, locks or I/O. Scope lifetimes must be nested.
// The whole callback is scope zero; queue submission is outside measured time.
class Block
{
public:
    Block(Session& session, BlockInfo info) noexcept;
    ~Block();
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    void setValue(std::size_t index, double value) noexcept;

private:
    friend class Scope;
    using Clock = std::chrono::steady_clock;
    struct Entry
    {
        std::size_t id = 0;
        Clock::time_point start;
        double childrenMs = 0.0;
    };
    bool begin(std::size_t id) noexcept;
    void end() noexcept;
    Session& m_session;
    Sample m_sample;
    std::array<Entry, maxScopes> m_stack;
    std::size_t m_depth = 0;
};

class Scope
{
public:
    Scope(Block& block, std::size_t id) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Block& m_block;
    bool m_active;
};
} // namespace diagnostics
