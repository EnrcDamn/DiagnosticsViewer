#include <diagnostics/Diagnostics.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <regex>
#include <stdexcept>
#include <thread>

namespace
{
thread_local bool watchAllocations = false;
thread_local std::size_t allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (watchAllocations)
        ++allocationCount;
    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
using namespace diagnostics;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Session file could not be read");
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

double number(const std::string& text, const std::string& key)
{
    const std::regex expression("\"" + key + "\":([0-9.eE+-]+)");
    std::sregex_iterator match(text.begin(), text.end(), expression), end;
    require(match != end, "Expected metric is missing");
    double value = 0.0;
    for (; match != end; ++match)
        value = std::stod((*match)[1].str());
    return value;
}

Config config(const char* name)
{
    Config result;
    result.application = name;
    result.directory = std::filesystem::current_path() / "test-sessions";
    result.scopes = { "callback", "outer", "inner" };
    result.values = { "block_size", "state" };
    return result;
}

std::string scopeRecord(const std::string& text, const std::string& name)
{
    const auto position = text.rfind("\"name\":\"" + name + "\"");
    require(position != std::string::npos, "Expected scope is missing");
    const auto start = text.rfind('{', position);
    return text.substr(start, text.find('}', position) - start + 1);
}

void timingAndShutdown()
{
    std::filesystem::path file;
    {
        Session session(config("Timing test"));
        file = session.file();
        session.log("Quotes: \"hello\"\n\\path", "warning");
        session.log(u8"Unicode: caffè");
        for (int i = 0; i < 3; ++i)
        {
            BlockInfo info;
            info.budgetMs = 1.0;
            info.values[0] = 128 + i;
            Block block(session, info);
            Scope outer(block, 1);
            {
                Scope inner(block, 2);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            block.setValue(1, 42);
        }
    }
    const auto text = read(file);
    require(scopeRecord(text, "callback").find("\"parents\":[]") != std::string::npos,
            "Whole block should not have a parent");
    require(scopeRecord(text, "outer").find("\"parents\":[0]") != std::string::npos,
            "Outer scope lost its block parent");
    require(scopeRecord(text, "inner").find("\"parents\":[1]") != std::string::npos,
            "Inner scope was not attached to its actual parent");
    require(number(text, "total_blocks") == 3, "Shutdown lost queued callbacks");
    require(number(text, "over_budget") == 3, "Over-budget callbacks not counted");
    require(number(scopeRecord(text, "callback"), "load") > 100,
            "Load must not be clamped at 100 percent");
    require(number(text, "p99_ms") >= 2, "P99 failed to include the slow callbacks");
    require(number(text, "block_size") == 130, "Actual block metadata was lost");
    require(number(text, "state") == 42, "Block value updates were lost");
    const auto outer = text.substr(text.rfind("\"name\":\"outer\""));
    const auto inner = text.substr(text.rfind("\"name\":\"inner\""));
    const auto root = text.substr(text.rfind("\"name\":\"callback\""));
    const auto firstNumber = [](const std::string& section, const char* key)
    {
        return number(section.substr(0, section.find('}')), key);
    };
    const auto total = firstNumber(root, "total_ms");
    const auto outerTotal = firstNumber(outer, "total_ms");
    require(std::abs(total - firstNumber(root, "self_ms") - outerTotal) < 0.00001,
            "Root self time did not exclude its measured child");
    require(std::abs(outerTotal - firstNumber(outer, "self_ms")
                     - firstNumber(inner, "total_ms")) < 0.00001,
            "Nested self time did not exclude its measured child");
    require(text.find("\\\"hello\\\"\\u000a\\\\path") != std::string::npos,
            "Log text was not JSON escaped");
    require(text.find(u8"caffè") != std::string::npos, "UTF-8 log text was corrupted");
    require(text.find("\"type\":\"closed\"") != std::string::npos,
            "Missing orderly shutdown marker");
}

void realtimeAndOverflow()
{
    std::filesystem::path file;
    constexpr std::size_t callbacks = 100000;
    {
        Session session(config("Overflow test"));
        file = session.file();
        allocationCount = 0;
        watchAllocations = true;
        for (std::size_t i = 0; i < callbacks; ++i)
        {
            BlockInfo info;
            info.budgetMs = 10;
            Block block(session, info);
            Scope first(block, 1);
            Scope nested(block, 2);
            block.setValue(0, static_cast<double>(i));
        }
        watchAllocations = false;
        require(allocationCount == 0, "Callback profiling allocated memory");
    }
    const auto text = read(file);
    const auto dropped = number(text, "dropped_blocks");
    require(dropped > 0, "Overflow path was not exercised");
    require(number(text, "total_blocks") + dropped == callbacks,
            "Queue lost or duplicated records without counting them");
}

void streamsAndMultipleInstances()
{
    auto settings = config("Stream test");
    settings.captureStandardStreams = true;
    auto first = std::make_unique<Session>(settings);
    const auto firstFile = first->file();
    std::filesystem::path secondFile;
    {
        Session second(settings);
        secondFile = second.file();
        std::cout << "Shared stdout message" << std::endl;
        std::cerr << "Shared stderr message" << std::endl;
        first.reset();
        std::cout << "Surviving instance message" << std::endl;
        second.log("Instance-specific message");
    }
    require(firstFile != secondFile, "Instances shared a file");
    const auto firstText = read(firstFile);
    const auto secondText = read(secondFile);
    require(firstText.find("Shared stdout message") != std::string::npos,
            "First instance missed stdout capture");
    require(secondText.find("Shared stderr message") != std::string::npos,
            "Second instance missed stderr capture");
    require(secondText.find("Surviving instance message") != std::string::npos,
            "Destroying an instance broke stream capture");
    require(firstText.find("Instance-specific message") == std::string::npos,
            "Explicit logs leaked between instances");
    std::cout << "Streams still work after sessions close" << std::endl;
}

void invalidAndEmptyBlocks()
{
    std::filesystem::path file;
    {
        Session session(config("Invalid values test"));
        file = session.file();
        BlockInfo info;
        info.budgetMs = std::numeric_limits<double>::infinity();
        info.values[0] = std::numeric_limits<double>::quiet_NaN();
        Block block(session, info);
        Scope invalid(block, maxScopes);
        block.setValue(1, std::numeric_limits<double>::infinity());
        block.setValue(maxValues, 123);
    }
    const auto text = read(file);
    for (const auto* name : { "callback", "outer", "inner" })
    {
        const auto record = scopeRecord(text, name);
        require(number(record, "load") == 0 && number(record, "peak_load") == 0,
                "Unknown budget should not divide by zero");
    }
    require(number(text, "block_size") == 0 && number(text, "state") == 0,
            "Non-finite values reached JSON output");
}

void hierarchyAndDepthLimit()
{
    auto settings = config("Hierarchy depth test");
    settings.scopes = { "callback", "one", "two", "three", "four", "five", "six", "seven" };
    std::filesystem::path file;
    {
        Session session(settings);
        file = session.file();
        {
            Block block(session, {});
            const auto nest = [&](const auto& self, std::size_t id) -> void
            {
                Scope scope(block, id);
                if (id + 1 < maxScopes)
                    self(self, id + 1);
                else
                {
                    Scope tooDeep(block, 1);
                    Scope invalidId(block, maxScopes);
                }
            };
            allocationCount = 0;
            watchAllocations = true;
            nest(nest, 1);
            watchAllocations = false;
            require(allocationCount == 0, "Recording nested parents allocated memory");
        }
        // Let both a populated report and an idle report reach the file.
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
    }
    const auto text = read(file);
    require(number(text, "blocks") == 0, "Idle hierarchy reporting was not exercised");
    for (std::size_t id = 1; id < maxScopes; ++id)
    {
        const auto record = scopeRecord(text, settings.scopes[id]);
        require(record.find("\"parents\":[" + std::to_string(id - 1) + "]") != std::string::npos,
                "Nested parent relationships were lost in an idle report");
    }
    const auto first = text.find("{\"type\":\"metrics\"");
    const auto second = text.find("{\"type\":\"metrics\"", first + 1);
    const auto measured = text.substr(first, second - first);
    require(number(scopeRecord(measured, "one"), "calls_per_block") == 1,
            "A scope beyond the nesting limit was recorded");
    require(number(scopeRecord(measured, "seven"), "calls_per_block") == 1,
            "The deepest supported scope was not recorded");
}

void sharedScopeParents()
{
    std::filesystem::path file;
    {
        Session session(config("Shared scope test"));
        file = session.file();
        Block block(session, {});
        {
            Scope outer(block, 1);
            Scope inner(block, 2);
        }
        Scope direct(block, 2);
    }
    const auto record = scopeRecord(read(file), "inner");
    require(record.find("\"parents\":[0,1]") != std::string::npos,
            "Using a scope under two parents lost a parent relationship");
    require(number(record, "calls_per_block") == 2,
            "Shared scope timings should still combine both calls");
}

void scopeLoadsAndIdleReports()
{
    auto settings = config("Scope load test");
    settings.scopes.push_back("unused");
    std::filesystem::path file;
    {
        Session session(settings);
        file = session.file();
        {
            BlockInfo info;
            info.budgetMs = 2.0;
            Block block(session, info);
            for (int call = 0; call < 2; ++call)
            {
                Scope outer(block, 1);
                Scope inner(block, 2);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        {
            BlockInfo info;
            info.budgetMs = 8.0;
            Block block(session, info);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
    }
    const auto text = read(file);
    const auto start = text.find("{\"type\":\"metrics\"");
    const auto measured = text.substr(start, text.find('\n', start) - start);
    const auto global = measured.substr(0, measured.find(",\"scopes\":"));
    require(number(global, "blocks") == 2, "Scope load test needs both block budgets");
    const auto root = scopeRecord(measured, "callback");
    require(std::abs(number(root, "load") - number(global, "load")) < 0.00001,
            "Whole-block scope average differs from the global average");
    require(std::abs(number(root, "peak_load") - number(global, "peak_load")) < 0.00001,
            "Whole-block scope peak differs from the global peak");
    for (const auto* name : { "outer", "inner" })
    {
        const auto record = scopeRecord(measured, name);
        const auto average = number(record, "load");
        const auto peak = number(record, "peak_load");
        const auto total = number(record, "total_ms");
        require(total > 0 && number(record, "calls_per_block") == 1,
                "Scope load test must include two calls in one block and an inactive block");
        require(std::abs(average - 100.0 * total * 2.0 / 10.0) < 0.00001,
                "Scope average did not use the sum of changing block budgets");
        require(std::abs(peak - 100.0 * total * 2.0 / 2.0) < 0.00001,
                "Scope peak did not combine calls within each block before applying its budget");
    }
    const auto unused = scopeRecord(measured, "unused");
    require(number(unused, "load") == 0 && number(unused, "peak_load") == 0,
            "Unexecuted scope should have zero load");
    require(number(text, "blocks") == 0, "Scope load test must include idle reports");
    for (const auto& name : settings.scopes)
    {
        const auto record = scopeRecord(text, name);
        require(number(record, "load") == 0 && number(record, "peak_load") == 0,
                "Idle reports retained stale scope loads");
    }
}
} // namespace

int main()
{
    try
    {
        timingAndShutdown();
        realtimeAndOverflow();
        streamsAndMultipleInstances();
        invalidAndEmptyBlocks();
        hierarchyAndDepthLimit();
        sharedScopeParents();
        scopeLoadsAndIdleReports();
        std::cout << "Diagnostics tests passed" << std::endl;
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
