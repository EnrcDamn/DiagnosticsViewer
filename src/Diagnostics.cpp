#include <diagnostics/Diagnostics.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <mutex>
#include <streambuf>
#include <thread>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#else
 #include <unistd.h>
#endif

namespace diagnostics
{
namespace
{
constexpr std::size_t queueCapacity = 1024;
constexpr std::size_t logCapacity = 128;
constexpr std::size_t messageCapacity = 512;
constexpr std::size_t percentileCapacity = 16384;
using Clock = std::chrono::steady_clock;
static_assert(std::atomic<std::size_t>::is_always_lock_free,
              "Diagnostics requires lock-free queue indexes");

double milliseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

void quoted(std::ostream& output, std::string_view text)
{
    output << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const auto character : text)
    {
        const auto c = static_cast<unsigned char>(character);
        if (c == '"' || c == '\\')
            output << '\\' << character;
        else if (c < 32)
            output << "\\u00" << hex[c >> 4] << hex[c & 15];
        else
            output << character;
    }
    output << '"';
}

struct Message
{
    std::array<char, messageCapacity> text {};
    std::array<char, 16> level {};
    Clock::time_point time;
};

// Only the non-realtime logging path uses this mutex.
struct LogQueue
{
    void push(std::string_view text, std::string_view level) noexcept
    {
        std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
        if (!lock.owns_lock() || count == logCapacity)
        {
            ++dropped;
            return;
        }
        auto& message = messages[(head + count) % logCapacity];
        const auto textSize = std::min(text.size(), message.text.size() - 1);
        const auto levelSize = std::min(level.size(), message.level.size() - 1);
        std::copy_n(text.data(), textSize, message.text.data());
        std::copy_n(level.data(), levelSize, message.level.data());
        message.text[textSize] = '\0';
        message.level[levelSize] = '\0';
        message.time = Clock::now();
        ++count;
    }

    bool pop(Message& message)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (count == 0)
            return false;
        message = messages[head];
        head = (head + 1) % logCapacity;
        --count;
        return true;
    }

    std::mutex mutex;
    std::array<Message, logCapacity> messages;
    std::size_t head = 0, count = 0;
    std::atomic<std::size_t> dropped { 0 };
};

// Installed once per module, restored at module shutdown. Weak destinations
// keep standard streams independent of any particular plugin instance lifetime.
class ConsoleCapture
{
    class Buffer final : public std::streambuf
    {
    public:
        Buffer(ConsoleCapture& owner, std::streambuf* original, const char* channel)
            : m_owner(owner), m_original(original), m_channel(channel) {}
        std::streambuf* original() const { return m_original; }

    private:
        std::streamsize xsputn(const char* text, std::streamsize count) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Preserve the original destination as well as capturing the text.
            m_original->sputn(text, count);
            for (std::streamsize i = 0; i < count; ++i)
            {
                if (text[i] == '\n')
                    publish();
                else
                {
                    m_pending[m_size++] = text[i];
                    if (m_size == m_pending.size())
                        publish();
                }
            }
            return count;
        }

        int_type overflow(int_type character) override
        {
            if (traits_type::eq_int_type(character, traits_type::eof()))
                return traits_type::not_eof(character);
            const auto c = traits_type::to_char_type(character);
            return xsputn(&c, 1) == 1 ? character : traits_type::eof();
        }

        int sync() override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            publish();
            m_original->pubsync();
            return 0;
        }

        void publish()
        {
            if (m_size != 0)
            {
                m_owner.publish({ m_pending.data(), m_size }, m_channel);
                m_size = 0;
            }
        }

        ConsoleCapture& m_owner;
        std::streambuf* m_original;
        const char* m_channel;
        std::mutex m_mutex;
        std::array<char, messageCapacity - 1> m_pending {};
        std::size_t m_size = 0;
    };

public:
    ConsoleCapture()
        : m_output(*this, std::cout.rdbuf(), "cout"),
          m_error(*this, std::cerr.rdbuf(), "cerr")
    {
        std::cout.rdbuf(&m_output);
        std::cerr.rdbuf(&m_error);
    }

    ~ConsoleCapture()
    {
        if (std::cout.rdbuf() == &m_output)
            std::cout.rdbuf(m_output.original());
        if (std::cerr.rdbuf() == &m_error)
            std::cerr.rdbuf(m_error.original());
    }

    void add(const std::shared_ptr<LogQueue>& queue)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_destinations.erase(std::remove_if(m_destinations.begin(), m_destinations.end(),
                                           [](const auto& item) { return item.expired(); }),
                             m_destinations.end());
        m_destinations.push_back(queue);
    }

    void publish(std::string_view text, std::string_view channel)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& destination : m_destinations)
            if (auto queue = destination.lock())
                queue->push(text, channel);
    }

private:
    Buffer m_output, m_error;
    std::mutex m_mutex;
    std::vector<std::weak_ptr<LogQueue>> m_destinations;
};

ConsoleCapture& consoleCapture()
{
    static ConsoleCapture capture;
    return capture;
}

struct Window
{
    void add(const Sample& sample)
    {
        ++count;
        last = sample.info;
        const auto elapsed = sample.scopes[0].totalMs;
        totalMs += elapsed;
        peakMs = std::max(peakMs, elapsed);
        if (sample.info.budgetMs > 0.0)
        {
            budgetMs += sample.info.budgetMs;
            peakLoad = std::max(peakLoad, 100.0 * elapsed / sample.info.budgetMs);
            if (elapsed > sample.info.budgetMs)
                ++overBudget;
        }
        if (percentileCount < durations.size())
            durations[percentileCount++] = elapsed;
        for (std::size_t i = 0; i < maxScopes; ++i)
        {
            scopes[i].totalMs += sample.scopes[i].totalMs;
            scopes[i].selfMs += sample.scopes[i].selfMs;
            scopes[i].calls += sample.scopes[i].calls;
            scopes[i].parents |= sample.scopes[i].parents;
        }
    }

    double p99()
    {
        if (percentileCount == 0)
            return 0.0;
        const auto rank = (99 * percentileCount + 99) / 100 - 1;
        std::nth_element(durations.begin(), durations.begin() + rank,
                         durations.begin() + percentileCount);
        return durations[rank];
    }

    std::size_t count = 0, percentileCount = 0, overBudget = 0;
    double totalMs = 0.0, peakMs = 0.0, budgetMs = 0.0, peakLoad = 0.0;
    BlockInfo last;
    std::array<ScopeStats, maxScopes> scopes {};
    std::array<double, percentileCapacity> durations {};
};
} // namespace

struct Session::Impl
{
    explicit Impl(Config settings) : config(std::move(settings))
    {
        if (config.scopes.empty())
            config.scopes.push_back("block");
        config.scopes.resize(std::min(config.scopes.size(), maxScopes));
        config.values.resize(std::min(config.values.size(), maxValues));
        std::error_code error;
        auto directory = config.directory;
        if (directory.empty())
            directory = std::filesystem::temp_directory_path(error) / "Diagnostics";
        if (!error)
            std::filesystem::create_directories(directory, error);
        static std::atomic<std::size_t> nextId { 0 };
        const auto id = ++nextId;
        const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
       #if defined(_WIN32)
        const auto pid = GetCurrentProcessId();
       #else
        const auto pid = getpid();
       #endif
        path = directory / ("session-" + std::to_string(pid) + "-" + std::to_string(id)
                            + "-" + std::to_string(stamp) + ".jsonl");
        if (!error)
            output.open(path, std::ios::binary);
        output.imbue(std::locale::classic());
        output << std::setprecision(9);
        output << "{\"type\":\"session\",\"application\":";
        quoted(output, config.application);
        output << ",\"pid\":" << pid << ",\"instance\":" << id << "}\n";
        output.flush();
        if (config.captureStandardStreams)
            consoleCapture().add(logs);
        worker = std::thread([this] { run(); });
        logs->push(output ? "Diagnostics started" : "Cannot open diagnostics file", "info");
    }

    ~Impl()
    {
        stopping.store(true, std::memory_order_release);
        worker.join();
    }

    void submit(const Sample& sample) noexcept
    {
        const auto write = writeIndex.load(std::memory_order_relaxed);
        const auto next = (write + 1) % queueCapacity;
        if (next == readIndex.load(std::memory_order_acquire))
        {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        queue[write] = sample;
        writeIndex.store(next, std::memory_order_release);
    }

    bool pop(Sample& sample)
    {
        const auto read = readIndex.load(std::memory_order_relaxed);
        if (read == writeIndex.load(std::memory_order_acquire))
            return false;
        sample = queue[read];
        readIndex.store((read + 1) % queueCapacity, std::memory_order_release);
        return true;
    }

    void report(Window& window, double time)
    {
        seen += window.count;
        overruns += window.overBudget;
        lifetimePeakMs = std::max(lifetimePeakMs, window.peakMs);
        output << "{\"type\":\"metrics\",\"time\":" << time
               << ",\"blocks\":" << window.count << ",\"total_blocks\":" << seen
               << ",\"mean_ms\":" << (window.count ? window.totalMs / window.count : 0.0)
               << ",\"p99_ms\":" << window.p99()
               << ",\"p99_samples\":" << window.percentileCount
               << ",\"peak_ms\":" << window.peakMs
               << ",\"lifetime_peak_ms\":" << lifetimePeakMs
               << ",\"load\":" << (window.budgetMs > 0.0
                                            ? 100.0 * window.totalMs / window.budgetMs : 0.0)
               << ",\"peak_load\":" << window.peakLoad
               << ",\"over_budget\":" << overruns
               << ",\"dropped_blocks\":" << dropped.load(std::memory_order_relaxed)
               << ",\"dropped_logs\":" << logs->dropped.load(std::memory_order_relaxed)
               << ",\"values\":{";
        for (std::size_t i = 0; i < config.values.size(); ++i)
        {
            if (i != 0)
                output << ',';
            quoted(output, config.values[i]);
            output << ':' << window.last.values[i];
        }
        output << "},\"scopes\":[";
        for (std::size_t i = 0; i < config.scopes.size(); ++i)
        {
            if (i != 0)
                output << ',';
            const auto& scope = window.scopes[i];
            scopeParents[i] |= scope.parents;
            output << "{\"id\":" << i << ",\"parents\":[";
            bool firstParent = true;
            for (std::size_t parent = 0; parent < config.scopes.size(); ++parent)
            {
                if ((scopeParents[i] & (1u << parent)) == 0)
                    continue;
                if (!firstParent)
                    output << ',';
                output << parent;
                firstParent = false;
            }
            output << "],\"name\":";
            quoted(output, config.scopes[i]);
            output << ",\"self_ms\":" << (window.count ? scope.selfMs / window.count : 0.0)
                   << ",\"total_ms\":" << (window.count ? scope.totalMs / window.count : 0.0)
                   << ",\"calls_per_block\":" << (window.count
                            ? static_cast<double>(scope.calls) / window.count : 0.0) << '}';
        }
        output << "]}\n";
    }

    void writeLog(const Message& message)
    {
        output << "{\"type\":\"log\",\"time\":" << milliseconds(message.time - started) / 1000.0
               << ",\"level\":";
        quoted(output, message.level.data());
        output << ",\"text\":";
        quoted(output, message.text.data());
        output << "}\n";
       #if defined(_WIN32)
        const auto text = "[" + config.application + " " + path.filename().string() + "] ["
            + message.level.data() + "] " + message.text.data() + "\n";
        OutputDebugStringA(text.c_str());
       #endif
    }

    void run()
    {
        Window window;
        auto lastReport = Clock::now();
        while (true)
        {
            Sample sample;
            // Bound work per pass, even when the producer continuously runs.
            for (std::size_t i = 0; i < queueCapacity && pop(sample); ++i)
            {
                window.add(sample);
                lastInfo = sample.info;
            }
            Message message;
            for (std::size_t i = 0; i < logCapacity && logs->pop(message); ++i)
                writeLog(message);
            const auto now = Clock::now();
            const auto stoppingNow = stopping.load(std::memory_order_acquire);
            if (now - lastReport >= std::chrono::milliseconds(250) || stoppingNow)
            {
                report(window, milliseconds(now - started) / 1000.0);
                window = Window {};
                window.last = lastInfo;
                lastReport = now;
                output.flush();
            }
            if (stoppingNow)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        output << "{\"type\":\"closed\"}\n";
        output.flush();
    }

    Config config;
    std::filesystem::path path;
    std::ofstream output;
    std::shared_ptr<LogQueue> logs = std::make_shared<LogQueue>();
    std::array<Sample, queueCapacity> queue;
    alignas(64) std::atomic<std::size_t> writeIndex { 0 };
    alignas(64) std::atomic<std::size_t> readIndex { 0 };
    std::atomic<std::size_t> dropped { 0 };
    std::atomic<bool> stopping { false };
    Clock::time_point started = Clock::now();
    std::thread worker;
    std::size_t seen = 0, overruns = 0;
    double lifetimePeakMs = 0.0;
    BlockInfo lastInfo;
    std::array<std::uint8_t, maxScopes> scopeParents {};
};

Session::Session(Config config) : m_impl(std::make_unique<Impl>(std::move(config))) {}
Session::~Session() = default;
void Session::log(std::string_view message, std::string_view level) noexcept
{
    m_impl->logs->push(message, level);
}
const std::filesystem::path& Session::file() const noexcept { return m_impl->path; }
void Session::submit(const Sample& sample) noexcept { m_impl->submit(sample); }

Block::Block(Session& session, BlockInfo info) noexcept : m_session(session)
{
    m_sample.info = info;
    if (!std::isfinite(m_sample.info.budgetMs) || m_sample.info.budgetMs < 0.0)
        m_sample.info.budgetMs = 0.0;
    for (auto& value : m_sample.info.values)
        if (!std::isfinite(value))
            value = 0.0;
    begin(0);
}
Block::~Block()
{
    while (m_depth != 0)
        end();
    m_session.submit(m_sample);
}
void Block::setValue(std::size_t index, double value) noexcept
{
    if (index < maxValues)
        m_sample.info.values[index] = std::isfinite(value) ? value : 0.0;
}
bool Block::begin(std::size_t id) noexcept
{
    if (id >= maxScopes || m_depth == m_stack.size())
        return false;
    if (m_depth != 0)
        m_sample.scopes[id].parents |= static_cast<std::uint8_t>(1u << m_stack[m_depth - 1].id);
    m_stack[m_depth++] = { id, Clock::now(), 0.0 };
    return true;
}
void Block::end() noexcept
{
    const auto now = Clock::now();
    const auto entry = m_stack[--m_depth];
    const auto elapsed = milliseconds(now - entry.start);
    auto& scope = m_sample.scopes[entry.id];
    scope.totalMs += elapsed;
    scope.selfMs += std::max(0.0, elapsed - entry.childrenMs);
    ++scope.calls;
    if (m_depth != 0)
        m_stack[m_depth - 1].childrenMs += elapsed;
}
Scope::Scope(Block& block, std::size_t id) noexcept
    : m_block(block), m_active(block.begin(id)) {}
Scope::~Scope()
{
    if (m_active)
        m_block.end();
}
} // namespace diagnostics
