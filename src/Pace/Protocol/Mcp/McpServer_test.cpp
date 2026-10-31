// SPDX-License-Identifier: Apache-2.0
#include <Pace/Core/Base64.hpp>
#include <Pace/Core/EnumTable.hpp>
#include <Pace/Core/Json.hpp>
#include <Pace/Core/Logger.hpp>
#include <Pace/Protocol/Mcp/McpServer.hpp>
#include <Pace/Protocol/Mcp/McpTool.hpp>
#include <Pace/Server/AdminHttpServer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Pace;
using namespace Pace::Mcp;

namespace
{

constexpr std::string_view Modern { "2026-07-28" };
constexpr std::string_view Legacy { "2025-11-25" };

constexpr std::string_view EchoInput {
    R"({"type":"object","properties":{"text":{"type":"string"},"count":{"type":"integer"}},"required":["text"]})"
};
constexpr std::string_view EchoOutput { R"({"type":"object","properties":{"echoed":{"type":"string"}}})" };

/// The fake table: a read-only tool that echoes, a destructive one that fails on request and throws
/// otherwise, and a mutating one that declares an output schema and answers without structured content.
std::vector<ToolRow> FakeTools()
{
    return {
        ToolRow { .name = "echo",
                  .title = "Echo",
                  .description = "Echoes its text.",
                  .inputSchema = EchoInput,
                  .outputSchema = EchoOutput,
                  .effect = ToolEffect::ReadOnly,
                  .handler =
                      [](ToolCall const& call) {
                          auto const& text = *call.arguments.Find("text")->AsString();
                          return ToolResult {
                              .structured = Json::Value { Json::Object {
                                  { "echoed", Json::Value { text } },
                                  { "client", Json::Value { std::string { call.clientName } } } } },
                              .text = std::format("echo: {}", text),
                          };
                      } },
        ToolRow { .name = "explode",
                  .title = "Explode",
                  .description = "Fails on request, throws otherwise.",
                  .inputSchema = R"({"type":"object"})",
                  .outputSchema = {},
                  .effect = ToolEffect::Destructive,
                  .handler = [](ToolCall const& call) -> ToolResult {
                      if (call.arguments.Find("fail"))
                          return ToolResult { .structured = {}, .text = "it failed as asked", .isError = true };
                      if (call.arguments.Find("quiet"))
                          return ToolResult { .structured = {}, .text = "done, nothing structured" };
                      throw std::runtime_error("boom");
                  } },
        ToolRow { .name = "hollow",
                  .title = "Hollow",
                  .description = "Declares an output schema and returns no structured content.",
                  .inputSchema = R"({"type":"object"})",
                  .outputSchema = EchoOutput,
                  .effect = ToolEffect::Mutating,
                  .handler = [](ToolCall const& call) -> ToolResult {
                      return ToolResult { .structured = {},
                                          .text = "nothing structured",
                                          .isError = call.arguments.Find("fail") != nullptr };
                  } },
    };
}

/// One HTTP request to the endpoint, owning everything the `AdminRequest` it builds borrows.
struct Exchange
{
    AdminMethod method { AdminMethod::Post };
    std::string body;
    std::array<std::string, EnumeratorCount<AdminHeader>> headerText {};

    Exchange& With(AdminHeader header, std::string_view value)
    {
        headerText[static_cast<std::size_t>(header)] = std::string { value };
        return *this;
    }

    Exchange& Without(AdminHeader header)
    {
        headerText[static_cast<std::size_t>(header)].clear();
        return *this;
    }

    [[nodiscard]] AdminRequest Request() const
    {
        AdminRequest request;
        request.method = method;
        request.body = body;
        request.peerHost = "127.0.0.1";
        request.path = "/mcp";
        for (auto const header: Enumerators<AdminHeader>())
            request.headers[static_cast<std::size_t>(header)] = headerText[static_cast<std::size_t>(header)];
        return request;
    }
};

constexpr std::string_view DefaultMeta { R"("_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
                                         R"("io.modelcontextprotocol/clientInfo":{"name":"claude-code","version":"2.1.0"},)"
                                         R"("io.modelcontextprotocol/clientCapabilities":{}})" };

/// A well-formed modern request: body and the three mirrored headers agreeing.
/// @param method The JSON-RPC method.
/// @param extraParams Further `params` members, comma-led, e.g. `,"name":"echo"`.
/// @param name The tool name to mirror into `Mcp-Name`, empty for none.
Exchange ModernRequest(std::string_view method, std::string_view extraParams = {}, std::string_view name = {})
{
    Exchange exchange;
    exchange.body =
        std::format(R"({{"jsonrpc":"2.0","id":7,"method":"{}","params":{{{}{}}}}})", method, DefaultMeta, extraParams);
    exchange.With(AdminHeader::McpProtocolVersion, Modern).With(AdminHeader::McpMethod, method);
    if (!name.empty())
        exchange.With(AdminHeader::McpName, name);
    return exchange;
}

Exchange CallEcho(std::string_view arguments)
{
    return ModernRequest("tools/call", std::format(R"(,"name":"echo","arguments":{})", arguments), "echo");
}

/// A legacy request: the body carries no modern `_meta`, and only the version header is sent.
Exchange LegacyRequest(std::string_view method, std::string_view params = "{}")
{
    Exchange exchange;
    exchange.body = std::format(R"({{"jsonrpc":"2.0","id":"l-1","method":"{}","params":{}}})", method, params);
    exchange.With(AdminHeader::McpProtocolVersion, Legacy);
    return exchange;
}

struct Fixture
{
    CapturingLogger logger;
    McpServer server { FakeTools(), "9.8.7", logger };

    [[nodiscard]] AdminResponse Send(Exchange const& exchange) const
    {
        return server.Handle(exchange.Request());
    }
};

[[nodiscard]] Json::Value BodyOf(AdminResponse const& response)
{
    auto parsed = Json::Parse(response.body);
    REQUIRE(parsed.has_value());
    return *std::move(parsed);
}

[[nodiscard]] std::optional<std::int64_t> ErrorCodeOf(AdminResponse const& response)
{
    auto const body = BodyOf(response);
    auto const* error = body.Find("error");
    if (!error || !error->Find("code"))
        return std::nullopt;
    return error->Find("code")->AsInt();
}

[[nodiscard]] Json::Value ParsedSchema(std::string_view text)
{
    auto parsed = Json::Parse(text);
    REQUIRE(parsed.has_value());
    return *std::move(parsed);
}

[[nodiscard]] Json::Value const& ResultOf(Json::Value const& body)
{
    auto const* result = body.Find("result");
    REQUIRE(result != nullptr);
    return *result;
}

[[nodiscard]] std::string StringAt(Json::Value const& value, std::string_view key)
{
    auto const* member = value.Find(key);
    REQUIRE(member != nullptr);
    REQUIRE(member->AsString() != nullptr);
    return *member->AsString();
}

[[nodiscard]] bool HasHeaderNamed(AdminResponse const& response, std::string_view name)
{
    return std::ranges::any_of(response.extraHeaders, [name](std::string const& line) {
        return line.size() >= name.size() && std::ranges::equal(line.substr(0, name.size()), name, [](char a, char b) {
                   auto const lower = [](char c) {
                       return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
                   };
                   return lower(a) == lower(b);
               });
    });
}

} // namespace

TEST_CASE("mcp-server modern requests must carry the mirrored headers", "[mcp]")
{
    Fixture const fixture;

    SECTION("a well-formed request is served")
    {
        auto const response = fixture.Send(ModernRequest("tools/list"));
        CHECK(response.status == "200 OK");
    }
    SECTION("no MCP-Protocol-Version header")
    {
        auto const response = fixture.Send(ModernRequest("tools/list").Without(AdminHeader::McpProtocolVersion));
        CHECK(response.status == "400 Bad Request");
        CHECK(ErrorCodeOf(response) == -32020);
    }
    SECTION("an MCP-Protocol-Version header disagreeing with _meta")
    {
        auto const response = fixture.Send(ModernRequest("tools/list").With(AdminHeader::McpProtocolVersion, "2099-01-01"));
        CHECK(response.status == "400 Bad Request");
        CHECK(ErrorCodeOf(response) == -32020);
    }
    SECTION("no Mcp-Method header")
    {
        auto const response = fixture.Send(ModernRequest("tools/list").Without(AdminHeader::McpMethod));
        CHECK(response.status == "400 Bad Request");
        CHECK(ErrorCodeOf(response) == -32020);
    }
    SECTION("an Mcp-Method header disagreeing with the body")
    {
        auto const response = fixture.Send(ModernRequest("tools/list").With(AdminHeader::McpMethod, "tools/call"));
        CHECK(response.status == "400 Bad Request");
        CHECK(ErrorCodeOf(response) == -32020);
    }
    SECTION("tools/call without Mcp-Name")
    {
        auto const response = fixture.Send(CallEcho(R"({"text":"hi"})").Without(AdminHeader::McpName));
        CHECK(response.status == "400 Bad Request");
        CHECK(ErrorCodeOf(response) == -32020);
    }
    SECTION("tools/call with an Mcp-Name disagreeing with params.name")
    {
        auto const response = fixture.Send(CallEcho(R"({"text":"hi"})").With(AdminHeader::McpName, "explode"));
        CHECK(response.status == "400 Bad Request");
        CHECK(ErrorCodeOf(response) == -32020);
    }
    SECTION("a request with no version header and no _meta version is a malformed MODERN request")
    {
        Exchange exchange;
        exchange.body = R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})";
        auto const response = fixture.Send(exchange);
        CHECK(response.status == "400 Bad Request");
        CHECK(ErrorCodeOf(response) == -32020);
        CHECK(fixture.logger.Snapshot().empty());
    }
}

TEST_CASE("mcp-server decodes the base64 sentinel in Mcp-Name before comparing", "[mcp]")
{
    Fixture const fixture;

    SECTION("the encoded name matches")
    {
        auto const encoded =
            std::format("=?base64?{}?=", Base64Encode(std::as_bytes(std::span { std::string_view { "echo" } })));
        auto const response = fixture.Send(CallEcho(R"({"text":"hi"})").With(AdminHeader::McpName, encoded));
        CHECK(response.status == "200 OK");
        CHECK(ErrorCodeOf(response) == std::nullopt);
    }
    SECTION("an encoded name that differs is a mismatch")
    {
        auto const encoded =
            std::format("=?base64?{}?=", Base64Encode(std::as_bytes(std::span { std::string_view { "explode" } })));
        auto const response = fixture.Send(CallEcho(R"({"text":"hi"})").With(AdminHeader::McpName, encoded));
        CHECK(ErrorCodeOf(response) == -32020);
    }
    SECTION("a sentinel holding invalid base64 is malformed")
    {
        auto const response = fixture.Send(CallEcho(R"({"text":"hi"})").With(AdminHeader::McpName, "=?base64?!!!?="));
        CHECK(response.status == "400 Bad Request");
        CHECK(ErrorCodeOf(response) == -32020);
    }
}

TEST_CASE("mcp-server refuses a modern request missing its required _meta fields", "[mcp]")
{
    Fixture const fixture;

    SECTION("no protocolVersion in _meta")
    {
        Exchange exchange;
        exchange.body =
            R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/clientCapabilities":{}}}})";
        exchange.With(AdminHeader::McpProtocolVersion, Modern).With(AdminHeader::McpMethod, "tools/list");
        auto const response = fixture.Send(exchange);
        CHECK(response.status == "400 Bad Request");
        CHECK(ErrorCodeOf(response) == -32602);
    }
    SECTION("no clientCapabilities in _meta")
    {
        Exchange exchange;
        exchange.body =
            R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})";
        exchange.With(AdminHeader::McpProtocolVersion, Modern).With(AdminHeader::McpMethod, "tools/list");
        auto const response = fixture.Send(exchange);
        CHECK(response.status == "400 Bad Request");
        CHECK(ErrorCodeOf(response) == -32602);
    }
}

TEST_CASE("mcp-server refuses an unsupported version and names what it supports", "[mcp]")
{
    Fixture const fixture;
    Exchange exchange;
    exchange.body =
        R"({"jsonrpc":"2.0","id":3,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"1900-01-01","io.modelcontextprotocol/clientCapabilities":{}}}})";
    exchange.With(AdminHeader::McpProtocolVersion, "1900-01-01").With(AdminHeader::McpMethod, "tools/list");

    auto const response = fixture.Send(exchange);
    CHECK(response.status == "400 Bad Request");
    auto const body = BodyOf(response);
    auto const& error = *body.Find("error");
    CHECK(error.Find("code")->AsInt() == -32022);
    auto const& data = *error.Find("data");
    CHECK(StringAt(data, "requested") == "1900-01-01");
    auto const& supported = *data.Find("supported")->AsArray();
    REQUIRE(supported.size() == SupportedVersions.size());
    CHECK(*supported[0].AsString() == SupportedVersions[0]);
    CHECK(*supported[1].AsString() == SupportedVersions[1]);
    CHECK(body.Find("id")->AsInt() == 3);
}

TEST_CASE("mcp-server server/discover reports versions, capabilities and serverInfo", "[mcp]")
{
    Fixture const fixture;
    auto const response = fixture.Send(ModernRequest("server/discover"));
    CHECK(response.status == "200 OK");
    auto const body = BodyOf(response);
    CHECK(body.Find("id")->AsInt() == 7);
    CHECK(StringAt(body, "jsonrpc") == "2.0");
    auto const& result = ResultOf(body);
    CHECK(StringAt(result, "resultType") == "complete");
    auto const& versions = *result.Find("supportedVersions")->AsArray();
    REQUIRE(versions.size() == 2);
    CHECK(*versions[0].AsString() == "2026-07-28");
    CHECK(*versions[1].AsString() == "2025-11-25");
    CHECK(result.Find("capabilities")->Find("tools")->Find("listChanged")->AsBool() == false);
    auto const& info = *result.Find("_meta")->Find("io.modelcontextprotocol/serverInfo");
    CHECK(StringAt(info, "name") == ServerName);
    CHECK(StringAt(info, "version") == "9.8.7");
    CHECK(result.Find("ttlMs")->AsInt().has_value());
    CHECK(result.Find("cacheScope")->AsString() != nullptr);
}

TEST_CASE("mcp-server tools/list reports the table in order with annotations and schemas", "[mcp]")
{
    Fixture const fixture;
    auto const body = BodyOf(fixture.Send(ModernRequest("tools/list")));
    auto const& result = ResultOf(body);
    CHECK(StringAt(result, "resultType") == "complete");
    auto const& tools = *result.Find("tools")->AsArray();
    REQUIRE(tools.size() == 3);

    auto const& echo = tools[0];
    CHECK(StringAt(echo, "name") == "echo");
    CHECK(StringAt(echo, "title") == "Echo");
    CHECK(StringAt(echo, "description") == "Echoes its text.");
    CHECK(Json::Serialize(*echo.Find("inputSchema")) == Json::Serialize(ParsedSchema(EchoInput)));
    CHECK(Json::Serialize(*echo.Find("outputSchema")) == Json::Serialize(ParsedSchema(EchoOutput)));
    CHECK(echo.Find("annotations")->Find("readOnlyHint")->AsBool() == true);
    CHECK(echo.Find("annotations")->Find("destructiveHint")->AsBool() == false);

    auto const& explode = tools[1];
    CHECK(StringAt(explode, "name") == "explode");
    CHECK(explode.Find("outputSchema") == nullptr);
    CHECK(explode.Find("annotations")->Find("readOnlyHint")->AsBool() == false);
    CHECK(explode.Find("annotations")->Find("destructiveHint")->AsBool() == true);

    // `Mutating` is the row whose two hints differ from both neighbours: it changes something, and
    // removes nothing, so a client prompts without warning of loss.
    auto const& hollow = tools[2];
    CHECK(StringAt(hollow, "name") == "hollow");
    CHECK(hollow.Find("annotations")->Find("readOnlyHint")->AsBool() == false);
    CHECK(hollow.Find("annotations")->Find("destructiveHint")->AsBool() == false);
}

TEST_CASE("mcp-server tools/call of an unknown tool is a protocol error", "[mcp]")
{
    Fixture const fixture;
    auto const response = fixture.Send(ModernRequest("tools/call", R"(,"name":"nope","arguments":{})", "nope"));
    CHECK(ErrorCodeOf(response) == -32602);
    CHECK(BodyOf(response).Find("result") == nullptr);
}

TEST_CASE("mcp-server tools/call validates required keys and primitive types as a tool error", "[mcp]")
{
    Fixture const fixture;

    auto const toolError = [&fixture](std::string_view arguments) {
        auto const response = fixture.Send(CallEcho(arguments));
        CHECK(response.status == "200 OK");
        auto const body = BodyOf(response);
        auto const& result = ResultOf(body);
        CHECK(result.Find("isError")->AsBool() == true);
        auto const& content = *result.Find("content")->AsArray();
        REQUIRE(content.size() == 1);
        return StringAt(content[0], "text");
    };

    SECTION("a missing required key is named")
    {
        CHECK(toolError("{}").find("'text'") != std::string::npos);
    }
    SECTION("a wrong primitive type is named")
    {
        CHECK(toolError(R"({"text":5})").find("'text'") != std::string::npos);
        CHECK(toolError(R"({"text":"x","count":"many"})").find("'count'") != std::string::npos);
        CHECK(toolError(R"({"text":"x","count":1.5})").find("'count'") != std::string::npos);
    }
    SECTION("an integral double is an integer")
    {
        auto const body = BodyOf(fixture.Send(CallEcho(R"({"text":"x","count":2.0})")));
        CHECK(ResultOf(body).Find("isError")->AsBool() == false);
    }
}

TEST_CASE("mcp-server tools/call result carries resultType, structuredContent, one text block and isError", "[mcp]")
{
    Fixture const fixture;
    auto const body = BodyOf(fixture.Send(CallEcho(R"({"text":"hello"})")));
    auto const& result = ResultOf(body);
    CHECK(StringAt(result, "resultType") == "complete");
    CHECK(StringAt(*result.Find("structuredContent"), "echoed") == "hello");
    CHECK(StringAt(*result.Find("structuredContent"), "client") == "claude-code");
    auto const& content = *result.Find("content")->AsArray();
    REQUIRE(content.size() == 1);
    CHECK(StringAt(content[0], "type") == "text");
    CHECK(StringAt(content[0], "text") == "echo: hello");
    CHECK(result.Find("isError")->AsBool() == false);

    SECTION("a handler's own isError reaches the client, with no structuredContent")
    {
        auto const failed =
            BodyOf(fixture.Send(ModernRequest("tools/call", R"(,"name":"explode","arguments":{"fail":true})", "explode")));
        CHECK(ResultOf(failed).Find("isError")->AsBool() == true);
        CHECK(ResultOf(failed).Find("structuredContent") == nullptr);
    }
}

TEST_CASE("mcp-server an unknown method is 404 with -32601", "[mcp]")
{
    Fixture const fixture;
    auto const response = fixture.Send(ModernRequest("resources/list"));
    CHECK(response.status == "404 Not Found");
    CHECK(ErrorCodeOf(response) == -32601);
}

TEST_CASE("mcp-server a notification is 202 with an empty body", "[mcp]")
{
    Fixture const fixture;
    Exchange exchange;
    exchange.body = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
    auto const response = fixture.Send(exchange);
    CHECK(response.status == "202 Accepted");
    CHECK(response.body.empty());
}

TEST_CASE("mcp-server a body that is not JSON is 400 with -32700", "[mcp]")
{
    Fixture const fixture;
    auto exchange = ModernRequest("tools/list");
    exchange.body = "{not json";
    auto const response = fixture.Send(exchange);
    CHECK(response.status == "400 Bad Request");
    CHECK(ErrorCodeOf(response) == -32700);
    CHECK(BodyOf(response).Find("id") == nullptr);
}

TEST_CASE("mcp-server a malformed JSON-RPC envelope is 400 with -32600", "[mcp]")
{
    Fixture const fixture;
    for (auto const* body: { R"([1,2])",
                             R"({"jsonrpc":"1.0","id":1,"method":"tools/list"})",
                             R"({"jsonrpc":"2.0","id":1})",
                             R"({"jsonrpc":"2.0","id":{},"method":"tools/list"})",
                             R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":[]})" })
    {
        Exchange exchange;
        exchange.body = body;
        auto const response = fixture.Send(exchange);
        INFO(body);
        CHECK(response.status == "400 Bad Request");
        CHECK(ErrorCodeOf(response) == -32600);
    }
}

TEST_CASE("mcp-server a non-loopback Origin is 403", "[mcp]")
{
    Fixture const fixture;

    for (auto const* origin: { "http://evil.example",
                               "http://localhost.evil.example",
                               "https://localhost",
                               "http://127.0.0.1:",
                               "http://127.0.0.1:99999",
                               "http://127.0.0.1:80x",
                               "null",
                               "http://[::1]evil" })
    {
        INFO(origin);
        auto const response = fixture.Send(ModernRequest("tools/list").With(AdminHeader::Origin, origin));
        CHECK(response.status == "403 Forbidden");
        CHECK(response.contentType == "application/json");
    }
    for (auto const* origin: { "http://127.0.0.1",
                               "http://127.0.0.1:8080",
                               "http://localhost",
                               "http://localhost:3000",
                               "http://[::1]",
                               "http://[::1]:6680" })
    {
        INFO(origin);
        auto const response = fixture.Send(ModernRequest("tools/list").With(AdminHeader::Origin, origin));
        CHECK(response.status == "200 OK");
    }
}

TEST_CASE("mcp-server never mints or echoes a session id", "[mcp]")
{
    Fixture const fixture;
    for (auto const& response: { fixture.Send(LegacyRequest("initialize", R"({"protocolVersion":"2025-11-25"})")),
                                 fixture.Send(ModernRequest("tools/list")),
                                 fixture.Send(LegacyRequest("tools/list")) })
    {
        CHECK(response.status == "200 OK");
        CHECK_FALSE(HasHeaderNamed(response, "Mcp-Session-Id"));
        CHECK(response.body.find("Mcp-Session-Id") == std::string::npos);
    }
}

TEST_CASE("mcp-server legacy initialize answers 2025-11-25 with capabilities and serverInfo", "[mcp]")
{
    Fixture const fixture;
    Exchange exchange;
    // A legacy client sends no version header on `initialize`: the method alone selects the era.
    exchange.body =
        R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"claude-code","version":"2.1.0"}}})";
    auto const response = fixture.Send(exchange);
    CHECK(response.status == "200 OK");
    auto const body = BodyOf(response);
    auto const& result = ResultOf(body);
    CHECK(StringAt(result, "protocolVersion") == "2025-11-25");
    CHECK(result.Find("capabilities")->Find("tools")->Find("listChanged")->AsBool() == false);
    CHECK(StringAt(*result.Find("serverInfo"), "name") == ServerName);
    CHECK(StringAt(*result.Find("serverInfo"), "version") == "9.8.7");
    CHECK(result.Find("resultType") == nullptr);
    CHECK(response.extraHeaders.empty());

    SECTION("initialize carrying modern _meta is a modern request, and modern has no initialize")
    {
        auto const modern = fixture.Send(ModernRequest("initialize"));
        CHECK(modern.status == "404 Not Found");
        CHECK(ErrorCodeOf(modern) == -32601);
    }
}

TEST_CASE("mcp-server legacy tools/list and tools/call are served from the same table without resultType", "[mcp]")
{
    Fixture const fixture;

    auto const list = BodyOf(fixture.Send(LegacyRequest("tools/list")));
    CHECK(StringAt(list, "id") == "l-1");
    auto const& listed = ResultOf(list);
    CHECK(listed.Find("resultType") == nullptr);
    CHECK(listed.Find("ttlMs") == nullptr);
    REQUIRE(listed.Find("tools")->AsArray()->size() == 3);
    CHECK(StringAt((*listed.Find("tools")->AsArray())[0], "name") == "echo");

    auto const call = BodyOf(fixture.Send(LegacyRequest("tools/call", R"({"name":"echo","arguments":{"text":"old"}})")));
    auto const& called = ResultOf(call);
    CHECK(called.Find("resultType") == nullptr);
    CHECK(StringAt(*called.Find("structuredContent"), "echoed") == "old");
    CHECK(StringAt(*called.Find("structuredContent"), "client").empty());
    CHECK(called.Find("isError")->AsBool() == false);

    SECTION("a modern-only method is not offered to the legacy era")
    {
        auto const discover = fixture.Send(LegacyRequest("server/discover"));
        CHECK(discover.status == "404 Not Found");
        CHECK(ErrorCodeOf(discover) == -32601);
    }
    SECTION("a legacy header beside a modern _meta version is a mismatch, not a legacy request")
    {
        auto const mixed = fixture.Send(ModernRequest("tools/list").With(AdminHeader::McpProtocolVersion, Legacy));
        CHECK(mixed.status == "400 Bad Request");
        CHECK(ErrorCodeOf(mixed) == -32020);
    }
}

TEST_CASE("mcp-server every response is application/json", "[mcp]")
{
    Fixture const fixture;
    auto notification = Exchange {};
    notification.body = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
    auto garbage = Exchange {};
    garbage.body = "]";
    for (auto const& response: { fixture.Send(ModernRequest("server/discover")),
                                 fixture.Send(ModernRequest("tools/list")),
                                 fixture.Send(CallEcho(R"({"text":"x"})")),
                                 fixture.Send(ModernRequest("nope/nope")),
                                 fixture.Send(notification),
                                 fixture.Send(garbage),
                                 fixture.Send(LegacyRequest("initialize")) })
    {
        CHECK(response.contentType == "application/json");
    }
}

TEST_CASE("mcp-server a handler that throws is a tool error in a 200, logged, and the server keeps serving", "[mcp]")
{
    // Ruling R8: a non-2xx is read by an HTTP MCP client as a transport failure, so the model would
    // never see which tool failed. A tool EXECUTION error is what the model can read and report.
    Fixture const fixture;
    auto const response = fixture.Send(ModernRequest("tools/call", R"(,"name":"explode","arguments":{})", "explode"));
    CHECK(response.status == "200 OK");
    auto const body = BodyOf(response);
    CHECK(body.Find("error") == nullptr);
    CHECK(body.Find("id")->AsInt() == 7);
    auto const& result = ResultOf(body);
    CHECK(result.Find("isError")->AsBool() == true);
    CHECK(result.Find("structuredContent") == nullptr);
    auto const& content = *result.Find("content")->AsArray();
    REQUIRE(content.size() == 1);
    CHECK(StringAt(content[0], "text") == "internal error in explode; see the node log");
    // The exception's own text stays in the log: it is a programmer error, not something to hand a model.
    CHECK(response.body.find("boom") == std::string::npos);

    auto const records = fixture.logger.Snapshot();
    REQUIRE(records.size() == 1);
    CHECK(records[0].level == LogLevel::Error);
    CHECK(records[0].message.find("explode") != std::string::npos);
    CHECK(records[0].message.find("boom") != std::string::npos);

    auto const next = fixture.Send(CallEcho(R"({"text":"still here"})"));
    CHECK(next.status == "200 OK");
    CHECK(StringAt(*ResultOf(BodyOf(next)).Find("structuredContent"), "echoed") == "still here");
}

TEST_CASE("mcp-server refuses a success without the structured content its output schema promises", "[mcp]")
{
    // Ruling R9: a tool declaring an `outputSchema` that answers success with no structured content
    // is a wrong answer that LOOKS right to a client reading only the text -- and one a client
    // enforcing output schemas rejects outright. So it is answered as the tool error it is.
    Fixture const fixture;
    auto const response = fixture.Send(ModernRequest("tools/call", R"(,"name":"hollow","arguments":{})", "hollow"));
    CHECK(response.status == "200 OK");
    auto const responseBody = BodyOf(response);
    auto const& result = ResultOf(responseBody);
    CHECK(result.Find("isError")->AsBool() == true);
    CHECK(result.Find("structuredContent") == nullptr);
    auto const text = StringAt((*result.Find("content")->AsArray())[0], "text");
    CHECK(text.find("hollow") != std::string::npos);
    CHECK(text != "nothing structured");

    auto const records = fixture.logger.Snapshot();
    REQUIRE(records.size() == 1);
    CHECK(records[0].level == LogLevel::Error);
    CHECK(records[0].message.find("hollow") != std::string::npos);

    SECTION("a tool ERROR owes no structured content, so it passes through unchanged")
    {
        auto const failed =
            fixture.Send(ModernRequest("tools/call", R"(,"name":"hollow","arguments":{"fail":true})", "hollow"));
        auto const failedBody = BodyOf(failed);
        auto const& failedResult = ResultOf(failedBody);
        CHECK(failedResult.Find("isError")->AsBool() == true);
        CHECK(StringAt((*failedResult.Find("content")->AsArray())[0], "text") == "nothing structured");
    }
    SECTION("a tool with no output schema may answer success without structured content")
    {
        auto const plain =
            fixture.Send(ModernRequest("tools/call", R"(,"name":"explode","arguments":{"quiet":true})", "explode"));
        auto const plainBody = BodyOf(plain);
        auto const& plainResult = ResultOf(plainBody);
        CHECK(plainResult.Find("isError")->AsBool() == false);
        CHECK(StringAt((*plainResult.Find("content")->AsArray())[0], "text") == "done, nothing structured");
        CHECK(fixture.logger.Snapshot().size() == 1);
    }
}

TEST_CASE("mcp-server legacy ping answers an empty result object", "[mcp]")
{
    Fixture const fixture;
    auto const response = fixture.Send(LegacyRequest("ping"));
    CHECK(response.status == "200 OK");
    auto const body = BodyOf(response);
    CHECK(StringAt(body, "id") == "l-1");
    auto const& result = ResultOf(body);
    REQUIRE(result.AsObject() != nullptr);
    CHECK(result.AsObject()->empty());
    CHECK(Json::Serialize(result) == "{}");
}

TEST_CASE("mcp-server a method other than POST is 405", "[mcp]")
{
    Fixture const fixture;
    auto exchange = ModernRequest("tools/list");
    exchange.method = AdminMethod::Get;
    auto const response = fixture.Send(exchange);
    CHECK(response.status == "405 Method Not Allowed");
    CHECK(HasHeaderNamed(response, "Allow"));
}

TEST_CASE("mcp-server refuses a tool row whose schema does not parse", "[mcp]")
{
    CapturingLogger logger;
    auto broken = FakeTools();
    broken[1].inputSchema = R"({"type":"object",})";
    CHECK_THROWS_AS(McpServer(std::move(broken), "1", logger), std::invalid_argument);

    auto notAnObject = FakeTools();
    notAnObject[0].outputSchema = "[]";
    CHECK_THROWS_AS(McpServer(std::move(notAnObject), "1", logger), std::invalid_argument);
}
