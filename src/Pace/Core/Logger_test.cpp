// SPDX-License-Identifier: Apache-2.0
#include <Pace/Core/Logger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <regex>
#include <sstream>
#include <string>

TEST_CASE("ConsoleLogger writes formatted lines to its sink", "[logger]")
{
    std::ostringstream sink;
    Pace::ConsoleLogger logger { sink, Pace::LogLevel::Trace, Pace::LogTimestamps::No };

    logger.Log(Pace::LogLevel::Info, "hello");
    logger.Log(Pace::LogLevel::Warn, "watch out");

    auto const output = sink.str();
    REQUIRE(output == "[INFO] hello\n[WARN] watch out\n");
}

TEST_CASE("ConsoleLogger respects MinLevel and SetMinLevel", "[logger]")
{
    std::ostringstream sink;
    Pace::ConsoleLogger logger { sink, Pace::LogLevel::Warn, Pace::LogTimestamps::No };

    logger.Log(Pace::LogLevel::Debug, "dropped");
    logger.Log(Pace::LogLevel::Info, "also dropped");
    logger.Log(Pace::LogLevel::Warn, "kept");

    REQUIRE(sink.str() == "[WARN] kept\n");

    sink.str("");
    logger.SetMinLevel(Pace::LogLevel::Debug);
    logger.Log(Pace::LogLevel::Debug, "now kept");
    REQUIRE(sink.str() == "[DEBUG] now kept\n");
}

TEST_CASE("NullLogger drops every record", "[logger]")
{
    Pace::NullLogger logger;
    logger.Log(Pace::LogLevel::Fatal, "ignored");
    // No assertion needed — surviving without UB is the contract.
    REQUIRE(logger.MinLevel() == Pace::LogLevel::Fatal);
}

TEST_CASE("CapturingLogger snapshots emitted records in order", "[logger]")
{
    Pace::CapturingLogger logger;
    logger.Log(Pace::LogLevel::Info, "first");
    logger.Log(Pace::LogLevel::Error, "second");

    auto const records = logger.Snapshot();
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].level == Pace::LogLevel::Info);
    REQUIRE(records[0].message == "first");
    REQUIRE(records[1].level == Pace::LogLevel::Error);
    REQUIRE(records[1].message == "second");
}

TEST_CASE("CapturingLogger Clear removes all records", "[logger]")
{
    Pace::CapturingLogger logger;
    logger.Log(Pace::LogLevel::Info, "one");
    logger.Log(Pace::LogLevel::Info, "two");
    logger.Clear();
    REQUIRE(logger.Snapshot().empty());
}

TEST_CASE("ConsoleLogger prefixes lines with ISO 8601 UTC timestamp when enabled", "[logger]")
{
    std::ostringstream sink;
    Pace::ConsoleLogger logger { sink, Pace::LogLevel::Trace, Pace::LogTimestamps::Yes };

    logger.Log(Pace::LogLevel::Info, "hello");

    auto const output = sink.str();
    static std::regex const pattern { R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+Z \[INFO\] hello\n)" };
    REQUIRE(std::regex_match(output, pattern));
}

TEST_CASE("ILogger::Logf only formats when the level passes the filter", "[logger]")
{
    Pace::CapturingLogger logger { Pace::LogLevel::Warn };
    logger.Logf(Pace::LogLevel::Debug, "dropped {}", 1);
    logger.Logf(Pace::LogLevel::Error, "kept {}", 42);

    auto const records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].message == "kept 42");
}

TEST_CASE("SourceLogger prefixes the source tag onto every record", "[logger][source]")
{
    Pace::CapturingLogger inner;
    Pace::SourceLogger logger { inner, "[203.0.113.7]" };

    logger.Log(Pace::LogLevel::Info, "hello");
    logger.Logf(Pace::LogLevel::Warn, "code {}", 7);

    auto const records = inner.Snapshot();
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].level == Pace::LogLevel::Info);
    REQUIRE(records[0].message == "[203.0.113.7] hello");
    REQUIRE(records[1].level == Pace::LogLevel::Warn);
    REQUIRE(records[1].message == "[203.0.113.7] code 7");
}

TEST_CASE("SourceLogger with an empty source forwards records unchanged", "[logger][source]")
{
    Pace::CapturingLogger inner;
    Pace::SourceLogger logger { inner, "" };

    logger.Log(Pace::LogLevel::Info, "untouched");

    auto const records = inner.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].message == "untouched");
}

TEST_CASE("SourceLogger forwards the level filter to its inner logger", "[logger][source]")
{
    // The inner logger owns the threshold; a below-threshold Logf must neither
    // format nor reach the inner sink, exactly as if logging directly.
    Pace::CapturingLogger inner { Pace::LogLevel::Warn };
    Pace::SourceLogger logger { inner, "[::1]" };

    REQUIRE(logger.MinLevel() == Pace::LogLevel::Warn);

    logger.Logf(Pace::LogLevel::Debug, "dropped");
    logger.Logf(Pace::LogLevel::Error, "kept");

    auto const records = inner.Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].message == "[::1] kept");

    // SetMinLevel is forwarded too.
    logger.SetMinLevel(Pace::LogLevel::Trace);
    REQUIRE(inner.MinLevel() == Pace::LogLevel::Trace);
}
