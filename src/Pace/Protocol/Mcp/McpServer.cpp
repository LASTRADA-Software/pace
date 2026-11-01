// SPDX-License-Identifier: Apache-2.0
#include <Pace/Core/Base64.hpp>
#include <Pace/Core/EnumTable.hpp>
#include <Pace/Protocol/Mcp/McpServer.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <initializer_list>
#include <stdexcept>
#include <utility>

#include <core/Ranges.hpp>

namespace Pace::Mcp
{

namespace
{

    using Json::Value;

    /// Which revision family a request belongs to.
    ///
    /// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
    enum class Era : std::uint8_t
    {
        Modern = 0, ///< `2026-07-28`: per-request `_meta`, mirrored headers, stateless.
        Legacy,     ///< `2025-11-25`: `initialize`, then requests under its version header.
        Last
    };

    /// A set of eras, one bit per `Era`.
    using EraMask = std::uint8_t;

    [[nodiscard]] constexpr EraMask MaskOf(Era era) noexcept
    {
        return static_cast<EraMask>(1U << static_cast<unsigned>(era));
    }

    constexpr EraMask BothEras = MaskOf(Era::Modern) | MaskOf(Era::Legacy);

    /// Which era each supported version speaks. One row per `SupportedVersions` entry, in its order.
    struct VersionRow
    {
        std::string_view version;
        Era era;
    };

    constexpr std::array VersionTable {
        VersionRow { .version = SupportedVersions[0], .era = Era::Modern },
        VersionRow { .version = SupportedVersions[1], .era = Era::Legacy },
    };
    static_assert(VersionTable.size() == SupportedVersions.size(),
                  "every supported version needs a row saying which era it speaks");

    [[nodiscard]] constexpr std::optional<Era> EraOfVersion(std::string_view version) noexcept
    {
        auto const* const row = core::findOrNull(VersionTable, version, &VersionRow::version);
        if (!row)
            return std::nullopt;
        return row->era;
    }

    /// The version a legacy `initialize` is answered with: the newest legacy one this server speaks.
    [[nodiscard]] consteval std::string_view LegacyVersion() noexcept
    {
        return core::findOrNull(VersionTable, Era::Legacy, &VersionRow::era)->version;
    }

    /// Every way a request is refused, and what each costs on the wire.
    ///
    /// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted. The wire carries the CODE and
    /// the STATUS of each row, never this ordinal.
    enum class Fault : std::uint8_t
    {
        ParseError = 0,     ///< The body is not JSON.
        InvalidRequest,     ///< JSON, but not a JSON-RPC request or notification.
        MethodNotAllowed,   ///< Not a `POST`: the transport has no other method.
        ForbiddenOrigin,    ///< A browser `Origin` that is not a loopback page.
        HeaderMismatch,     ///< A mirrored header missing, malformed or disagreeing with the body.
        MissingMeta,        ///< A modern request without its required `_meta` fields.
        UnsupportedVersion, ///< A protocol version this server does not speak.
        MethodNotFound,     ///< A method this server does not implement (in this era).
        InvalidParams,      ///< A method's parameters are malformed, or name an unknown tool.
        Internal,           ///< A programmer error in the envelope itself; a throwing TOOL is a tool error instead.
        Last
    };

    struct FaultRow
    {
        Fault fault;
        std::int64_t code;       ///< The JSON-RPC error code.
        std::string_view status; ///< The HTTP status line.
    };

    constexpr EnumTable<Fault, FaultRow> FaultTable { {
        { .fault = Fault::ParseError, .code = -32700, .status = "400 Bad Request" },
        { .fault = Fault::InvalidRequest, .code = -32600, .status = "400 Bad Request" },
        { .fault = Fault::MethodNotAllowed, .code = -32600, .status = "405 Method Not Allowed" },
        { .fault = Fault::ForbiddenOrigin, .code = -32600, .status = "403 Forbidden" },
        { .fault = Fault::HeaderMismatch, .code = -32020, .status = "400 Bad Request" },
        { .fault = Fault::MissingMeta, .code = -32602, .status = "400 Bad Request" },
        { .fault = Fault::UnsupportedVersion, .code = -32022, .status = "400 Bad Request" },
        { .fault = Fault::MethodNotFound, .code = -32601, .status = "404 Not Found" },
        // A method that was found and reached its handler answers its own refusal in a 200: the
        // transport only prescribes a status for the envelope failures above.
        { .fault = Fault::InvalidParams, .code = -32602, .status = "200 OK" },
        { .fault = Fault::Internal, .code = -32603, .status = "500 Internal Server Error" },
    } };
    static_assert(RowsInEnumeratorOrder(FaultTable, &FaultRow::fault));

    [[nodiscard]] FaultRow const& RowOf(Fault fault) noexcept
    {
        return FaultTable[static_cast<std::size_t>(fault)];
    }

    /// The `tools/list` annotation hints each effect renders as.
    struct EffectRow
    {
        ToolEffect effect;
        bool readOnlyHint;
        bool destructiveHint;
    };

    constexpr EnumTable<ToolEffect, EffectRow> EffectTable { {
        { .effect = ToolEffect::ReadOnly, .readOnlyHint = true, .destructiveHint = false },
        { .effect = ToolEffect::Mutating, .readOnlyHint = false, .destructiveHint = false },
        { .effect = ToolEffect::Destructive, .readOnlyHint = false, .destructiveHint = true },
    } };
    static_assert(RowsInEnumeratorOrder(EffectTable, &EffectRow::effect));

    constexpr std::string_view JsonRpcVersion { "2.0" };
    constexpr std::string_view ContentTypeJson { "application/json" };
    constexpr std::string_view AcceptedStatus { "202 Accepted" };
    constexpr std::string_view OkStatus { "200 OK" };
    constexpr std::string_view AllowHeader { "Allow: POST" };

    constexpr std::string_view MetaProtocolVersion { "io.modelcontextprotocol/protocolVersion" };
    constexpr std::string_view MetaClientCapabilities { "io.modelcontextprotocol/clientCapabilities" };
    constexpr std::string_view MetaClientInfo { "io.modelcontextprotocol/clientInfo" };
    constexpr std::string_view MetaServerInfo { "io.modelcontextprotocol/serverInfo" };

    /// How long a client may cache `server/discover` and `tools/list`. Zero -- re-fetch whenever needed --
    /// because the same loopback port may be served by another build after a restart, and both answers
    /// cost a few hundred bytes.
    constexpr std::int64_t ListingTtlMs = 0;
    /// Who may share a cached listing: this server answers one machine's own caller.
    constexpr std::string_view ListingCacheScope { "private" };

    /// The `Mcp-Name` sentinel for a value that is not a plain ASCII header value.
    constexpr std::string_view Base64SentinelPrefix { "=?base64?" };
    constexpr std::string_view Base64SentinelSuffix { "?=" };

    /// The browser origins a loopback MCP server accepts: `http://<host>[:<port>]` for each host.
    constexpr std::string_view LoopbackOriginScheme { "http://" };
    constexpr std::array LoopbackOriginHosts {
        std::string_view { "127.0.0.1" },
        std::string_view { "localhost" },
        std::string_view { "[::1]" },
    };
    constexpr std::uint32_t MaxPort = 65535;

    [[nodiscard]] Value Text(std::string_view text)
    {
        return Value { std::string { text } };
    }

    [[nodiscard]] Value Boolean(bool flag)
    {
        return Value { flag };
    }

    [[nodiscard]] Value Integer(std::int64_t number)
    {
        return Value { number };
    }

    [[nodiscard]] Value Obj(std::initializer_list<Json::Member> members)
    {
        return Value { Json::Object(members) };
    }

    [[nodiscard]] Value VersionList()
    {
        Json::Array versions;
        versions.reserve(SupportedVersions.size());
        std::ranges::transform(SupportedVersions, std::back_inserter(versions), Text);
        return Value { std::move(versions) };
    }

    [[nodiscard]] Value ToolsCapability()
    {
        return Obj({ { "tools", Obj({ { "listChanged", Boolean(false) } }) } });
    }

    /// A refusal: which one, the sentence that explains it, and its `data` (`null` for none).
    struct Failure
    {
        Fault fault;
        std::string message;
        Value data {};
    };

    [[nodiscard]] Failure Fail(Fault fault, std::string message)
    {
        return Failure { .fault = fault, .message = std::move(message) };
    }

    [[nodiscard]] AdminResponse JsonResponse(std::string_view status, Value const& body)
    {
        return AdminResponse { .status = status, .contentType = ContentTypeJson, .body = Json::Serialize(body) };
    }

    /// A JSON-RPC error response; @p id null leaves the `id` out, for a request whose id is unknown.
    [[nodiscard]] AdminResponse ErrorResponse(Value const* id, Failure const& failure)
    {
        auto const& row = RowOf(failure.fault);
        Json::Object error { { "code", Integer(row.code) }, { "message", Text(failure.message) } };
        if (!failure.data.IsNull())
            error.emplace_back("data", failure.data);
        Json::Object body { { "jsonrpc", Text(JsonRpcVersion) } };
        if (id)
            body.emplace_back("id", *id);
        body.emplace_back("error", Value { std::move(error) });
        auto response = JsonResponse(row.status, Value { std::move(body) });
        if (failure.fault == Fault::MethodNotAllowed)
            response.extraHeaders.emplace_back(AllowHeader);
        return response;
    }

    [[nodiscard]] bool IsPort(std::string_view digits) noexcept
    {
        std::uint32_t port = 0;
        auto const [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), port);
        return !digits.empty() && error == std::errc {} && end == digits.data() + digits.size() && port >= 1
               && port <= MaxPort && std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; });
    }

    /// Whether a browser `Origin` names a page served from this machine's loopback.
    [[nodiscard]] bool IsLoopbackOrigin(std::string_view origin) noexcept
    {
        if (!origin.starts_with(LoopbackOriginScheme))
            return false;
        auto const authority = origin.substr(LoopbackOriginScheme.size());
        return std::ranges::any_of(LoopbackOriginHosts, [authority](std::string_view host) {
            if (!authority.starts_with(host))
                return false;
            auto const tail = authority.substr(host.size());
            return tail.empty() || (tail.front() == ':' && IsPort(tail.substr(1)));
        });
    }

    /// A mirrored header's value as the client meant it: the `=?base64?…?=` sentinel decoded, anything
    /// else as sent. Nothing when the sentinel holds no valid base64.
    [[nodiscard]] std::optional<std::string> DecodeHeaderValue(std::string_view raw)
    {
        if (raw.size() >= Base64SentinelPrefix.size() + Base64SentinelSuffix.size() && raw.starts_with(Base64SentinelPrefix)
            && raw.ends_with(Base64SentinelSuffix))
        {
            return Base64Decode(raw.substr(Base64SentinelPrefix.size(),
                                           raw.size() - Base64SentinelPrefix.size() - Base64SentinelSuffix.size()));
        }
        return std::string { raw };
    }

    [[nodiscard]] std::string const* StringMember(Value const* object, std::string_view key) noexcept
    {
        if (!object)
            return nullptr;
        auto const* member = object->Find(key);
        return member ? member->AsString() : nullptr;
    }

    [[nodiscard]] bool IsIntegral(Value const& value) noexcept
    {
        if (value.AsInt() || value.AsUint())
            return true;
        auto const* real = std::get_if<double>(&value.v);
        return real && std::isfinite(*real) && *real == std::trunc(*real);
    }

    /// The JSON Schema `type` names an argument is checked against. A name not listed is not checked:
    /// this is a guard against a model's slips, not a JSON Schema engine.
    struct TypeRow
    {
        std::string_view name;
        bool (*matches)(Value const&) noexcept;
    };

    constexpr std::array TypeTable {
        TypeRow { .name = "string", .matches = [](Value const& v) noexcept { return v.AsString() != nullptr; } },
        TypeRow { .name = "integer", .matches = IsIntegral },
        TypeRow {
            .name = "number",
            .matches =
                [](Value const& v) noexcept { return v.AsInt() || v.AsUint() || std::holds_alternative<double>(v.v); } },
        TypeRow { .name = "boolean", .matches = [](Value const& v) noexcept { return v.AsBool().has_value(); } },
        TypeRow { .name = "object", .matches = [](Value const& v) noexcept { return v.AsObject() != nullptr; } },
        TypeRow { .name = "array", .matches = [](Value const& v) noexcept { return v.AsArray() != nullptr; } },
        TypeRow { .name = "null", .matches = [](Value const& v) noexcept { return v.IsNull(); } },
    };

    /// The first way @p arguments breaks @p schema's `required` keys or a property's primitive `type`.
    /// @return A sentence naming the field, or nothing when the arguments pass.
    [[nodiscard]] std::optional<std::string> ArgumentViolation(Value const& schema, Value const& arguments)
    {
        if (auto const* required = schema.Find("required"); required && required->AsArray())
        {
            for (auto const& key: *required->AsArray())
            {
                if (auto const* name = key.AsString(); name && !arguments.Find(*name))
                    return std::format("missing required argument '{}'", *name);
            }
        }
        auto const* properties = schema.Find("properties");
        if (!properties || !properties->AsObject())
            return std::nullopt;
        for (auto const& [name, property]: *properties->AsObject())
        {
            auto const* argument = arguments.Find(name);
            auto const* type = StringMember(&property, "type");
            if (!argument || !type)
                continue;
            auto const* const row = core::findOrNull(TypeTable, std::string_view { *type }, &TypeRow::name);
            if (row && !row->matches(*argument))
                return std::format("argument '{}' must be of type {}", name, *type);
        }
        return std::nullopt;
    }

    [[nodiscard]] Value RenderToolResult(ToolResult const& result, Era era)
    {
        Json::Object body;
        if (era == Era::Modern)
            body.emplace_back("resultType", Text("complete"));
        Json::Array content;
        content.push_back(Obj({ { "type", Text("text") }, { "text", Text(result.text) } }));
        body.emplace_back("content", Value { std::move(content) });
        if (!result.structured.IsNull())
            body.emplace_back("structuredContent", result.structured);
        body.emplace_back("isError", Boolean(result.isError));
        return Value { std::move(body) };
    }

    [[nodiscard]] Json::Value ParseSchema(std::string_view toolName, std::string_view text, std::string_view which)
    {
        auto parsed = Json::Parse(text);
        if (!parsed || !parsed->AsObject())
            throw std::invalid_argument(std::format("MCP tool '{}': its {} schema is not a JSON object", toolName, which));
        return *std::move(parsed);
    }

    /// Which body value the `Mcp-Name` header mirrors for a method.
    ///
    /// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
    enum class NameMirror : std::uint8_t
    {
        None = 0,   ///< The method carries no `Mcp-Name`.
        ParamsName, ///< `Mcp-Name` mirrors `params.name`.
    };

} // namespace

/// What a method handler answered: a result, or a refusal.
struct McpServer::Reply
{
    std::expected<Value, Failure> outcome;
};

/// The request once its envelope has been read.
struct McpServer::Envelope
{
    Era era;                     ///< Which revision family it speaks.
    Value const* params;         ///< `params`, or null when it carried none.
    std::string_view clientName; ///< The client's self-reported name, empty when unknown.
};

/// One row of the method table.
struct McpServer::MethodRow
{
    std::string_view method;                            ///< The JSON-RPC method name.
    EraMask eras;                                       ///< The eras that offer it.
    NameMirror name;                                    ///< What a modern `Mcp-Name` must mirror.
    Reply (McpServer::*handler)(Envelope const&) const; ///< Answers it.
};

McpServer::McpServer(std::vector<ToolRow> tools, std::string_view serverVersion, ILogger& logger):
    _serverVersion { serverVersion },
    _logger { logger }
{
    _tools.reserve(tools.size());
    for (auto& row: tools)
    {
        auto input = ParseSchema(row.name, row.inputSchema, "input");
        auto output =
            row.outputSchema.empty() ? std::nullopt : std::optional { ParseSchema(row.name, row.outputSchema, "output") };
        _tools.push_back(Tool { .row = std::move(row), .inputSchema = std::move(input), .outputSchema = std::move(output) });
    }
}

AdminResponse McpServer::Handle(AdminRequest const& request) const
{
    try
    {
        return HandleUnguarded(request);
    }
    catch (std::exception const& error)
    {
        _logger.Logf(LogLevel::Error, "mcp: request failed with an exception: {}", error.what());
    }
    catch (...)
    {
        _logger.Log(LogLevel::Error, "mcp: request failed with a non-standard exception");
    }
    return ErrorResponse(nullptr, Fail(Fault::Internal, "Internal error"));
}

AdminResponse McpServer::HandleUnguarded(AdminRequest const& request) const
{
    static constexpr std::array methodTable {
        MethodRow { .method = "initialize",
                    .eras = MaskOf(Era::Legacy),
                    .name = NameMirror::None,
                    .handler = &McpServer::Initialize },
        MethodRow { .method = "ping", .eras = MaskOf(Era::Legacy), .name = NameMirror::None, .handler = &McpServer::Ping },
        MethodRow { .method = "server/discover",
                    .eras = MaskOf(Era::Modern),
                    .name = NameMirror::None,
                    .handler = &McpServer::Discover },
        MethodRow { .method = "tools/list", .eras = BothEras, .name = NameMirror::None, .handler = &McpServer::ListTools },
        MethodRow {
            .method = "tools/call", .eras = BothEras, .name = NameMirror::ParamsName, .handler = &McpServer::CallTool },
    };

    if (request.method != AdminMethod::Post)
        return ErrorResponse(nullptr, Fail(Fault::MethodNotAllowed, "The MCP endpoint accepts POST only"));

    if (auto const origin = request.Header(AdminHeader::Origin); !origin.empty() && !IsLoopbackOrigin(origin))
        return ErrorResponse(nullptr, Fail(Fault::ForbiddenOrigin, std::format("Origin '{}' is not allowed", origin)));

    auto const parsed = Json::Parse(request.body, RequestParseLimits);
    if (!parsed)
        return ErrorResponse(nullptr, Fail(Fault::ParseError, "Parse error"));
    auto const& message = *parsed;

    auto const* id = message.Find("id");
    if (id && !id->AsString() && !id->AsInt())
        return ErrorResponse(nullptr, Fail(Fault::InvalidRequest, "Invalid request: id must be a string or an integer"));
    auto const* jsonrpc = StringMember(&message, "jsonrpc");
    auto const* method = StringMember(&message, "method");
    if (!message.AsObject() || !jsonrpc || *jsonrpc != JsonRpcVersion || !method)
        return ErrorResponse(id, Fail(Fault::InvalidRequest, "Invalid request: not a JSON-RPC 2.0 request"));
    auto const* params = message.Find("params");
    if (params && !params->AsObject())
        return ErrorResponse(id, Fail(Fault::InvalidRequest, "Invalid request: params must be an object"));

    // A notification asks for no answer, and this server acts on none.
    if (!id)
        return AdminResponse { .status = AcceptedStatus, .contentType = ContentTypeJson };

    auto const* meta = params ? params->Find("_meta") : nullptr;
    auto const* metaVersion = StringMember(meta, MetaProtocolVersion);
    auto const versionHeader = request.Header(AdminHeader::McpProtocolVersion);

    // The legacy era is chosen by exactly two signals: `initialize` without modern `_meta`, or a legacy
    // version header on a request that does not claim a modern version in its body.
    auto const legacy =
        (*method == "initialize" && !metaVersion)
        || (EraOfVersion(versionHeader) == Era::Legacy && (!metaVersion || EraOfVersion(*metaVersion) == Era::Legacy));
    auto const era = legacy ? Era::Legacy : Era::Modern;

    auto const fail = [id](Fault fault, std::string text) {
        return ErrorResponse(id, Fail(fault, std::move(text)));
    };

    std::string_view clientName;
    if (era == Era::Modern)
    {
        if (versionHeader.empty())
            return fail(Fault::HeaderMismatch, "Header mismatch: the MCP-Protocol-Version header is required");
        if (!metaVersion)
            return fail(Fault::MissingMeta, std::format("Invalid params: _meta '{}' is required", MetaProtocolVersion));
        if (versionHeader != *metaVersion)
            return fail(Fault::HeaderMismatch,
                        std::format("Header mismatch: MCP-Protocol-Version header value '{}' does not match body "
                                    "value '{}'",
                                    versionHeader,
                                    *metaVersion));
        if (EraOfVersion(*metaVersion) != Era::Modern)
        {
            auto failure = Fail(Fault::UnsupportedVersion, "Unsupported protocol version");
            failure.data = Obj({ { "supported", VersionList() }, { "requested", Text(*metaVersion) } });
            return ErrorResponse(id, failure);
        }
        auto const* capabilities = meta->Find(MetaClientCapabilities);
        if (!capabilities || !capabilities->AsObject())
            return fail(Fault::MissingMeta, std::format("Invalid params: _meta '{}' is required", MetaClientCapabilities));
        auto const methodHeader = request.Header(AdminHeader::McpMethod);
        if (methodHeader != *method)
            return fail(Fault::HeaderMismatch,
                        methodHeader.empty()
                            ? std::string { "Header mismatch: the Mcp-Method header is required" }
                            : std::format("Header mismatch: Mcp-Method header value '{}' does not match body value '{}'",
                                          methodHeader,
                                          *method));
        if (auto const* name = StringMember(meta->Find(MetaClientInfo), "name"))
            clientName = *name;
    }

    auto const* const row = core::findIfOrNull(methodTable, [&](MethodRow const& candidate) {
        return candidate.method == *method && (candidate.eras & MaskOf(era)) != 0;
    });
    if (!row)
        return fail(Fault::MethodNotFound, std::format("Method not found: {}", *method));

    if (era == Era::Modern && row->name == NameMirror::ParamsName)
    {
        auto const nameHeader = request.Header(AdminHeader::McpName);
        auto const* bodyName = StringMember(params, "name");
        if (nameHeader.empty())
            return fail(Fault::HeaderMismatch, "Header mismatch: the Mcp-Name header is required");
        auto const decoded = DecodeHeaderValue(nameHeader);
        if (!decoded)
            return fail(Fault::HeaderMismatch, "Header mismatch: the Mcp-Name header holds invalid base64");
        if (!bodyName || *decoded != *bodyName)
            return fail(Fault::HeaderMismatch,
                        std::format("Header mismatch: Mcp-Name header value '{}' does not match body value '{}'",
                                    *decoded,
                                    bodyName ? std::string_view { *bodyName } : std::string_view {}));
    }

    auto const reply = (this->*(row->handler))(Envelope { .era = era, .params = params, .clientName = clientName });
    if (!reply.outcome)
        return ErrorResponse(id, reply.outcome.error());
    return JsonResponse(OkStatus, Obj({ { "jsonrpc", Text(JsonRpcVersion) }, { "id", *id }, { "result", *reply.outcome } }));
}

Json::Value McpServer::ServerInfo() const
{
    return Obj({ { "name", Text(ServerName) }, { "version", Text(_serverVersion) } });
}

McpServer::Reply McpServer::Initialize(Envelope const& /*envelope*/) const
{
    return Reply { Obj({ { "protocolVersion", Text(LegacyVersion()) },
                         { "capabilities", ToolsCapability() },
                         { "serverInfo", ServerInfo() } }) };
}

McpServer::Reply McpServer::Ping(Envelope const& /*envelope*/) const
{
    return Reply { Obj({}) };
}

McpServer::Reply McpServer::Discover(Envelope const& /*envelope*/) const
{
    return Reply { Obj({ { "resultType", Text("complete") },
                         { "supportedVersions", VersionList() },
                         { "capabilities", ToolsCapability() },
                         { "_meta", Obj({ { std::string { MetaServerInfo }, ServerInfo() } }) },
                         { "ttlMs", Integer(ListingTtlMs) },
                         { "cacheScope", Text(ListingCacheScope) } }) };
}

McpServer::Reply McpServer::ListTools(Envelope const& envelope) const
{
    Json::Array tools;
    tools.reserve(_tools.size());
    for (auto const& tool: _tools)
    {
        auto const& effect = EffectTable[static_cast<std::size_t>(tool.row.effect)];
        Json::Object entry { { "name", Text(tool.row.name) },
                             { "title", Text(tool.row.title) },
                             { "description", Text(tool.row.description) },
                             { "inputSchema", tool.inputSchema } };
        if (tool.outputSchema)
            entry.emplace_back("outputSchema", *tool.outputSchema);
        entry.emplace_back("annotations",
                           Obj({ { "readOnlyHint", Boolean(effect.readOnlyHint) },
                                 { "destructiveHint", Boolean(effect.destructiveHint) } }));
        tools.emplace_back(std::move(entry));
    }
    Json::Object result;
    if (envelope.era == Era::Modern)
        result.emplace_back("resultType", Text("complete"));
    result.emplace_back("tools", Value { std::move(tools) });
    if (envelope.era == Era::Modern)
    {
        result.emplace_back("ttlMs", Integer(ListingTtlMs));
        result.emplace_back("cacheScope", Text(ListingCacheScope));
    }
    return Reply { Value { std::move(result) } };
}

McpServer::Tool const* McpServer::FindTool(std::string_view name) const noexcept
{
    return core::findOrNull(_tools, name, [](Tool const& tool) { return tool.row.name; });
}

McpServer::Reply McpServer::CallTool(Envelope const& envelope) const
{
    auto const* name = StringMember(envelope.params, "name");
    if (!name)
        return Reply { std::unexpected { Fail(Fault::InvalidParams, "Invalid params: tools/call requires a name") } };
    auto const* tool = FindTool(*name);
    if (!tool)
        return Reply { std::unexpected { Fail(Fault::InvalidParams, std::format("Unknown tool: {}", *name)) } };

    Value const noArguments { Json::Object {} };
    auto const* arguments = envelope.params->Find("arguments");
    if (!arguments)
        arguments = &noArguments;
    if (!arguments->AsObject())
        return Reply { std::unexpected { Fail(Fault::InvalidParams, "Invalid params: arguments must be an object") } };

    if (auto violation = ArgumentViolation(tool->inputSchema, *arguments))
    {
        return Reply { RenderToolResult(ToolResult { .structured = {},
                                                     .text = std::format("Invalid arguments for {}: {}", *name, *violation),
                                                     .isError = true },
                                        envelope.era) };
    }

    // A tool that failed in a way it did not report is answered as a tool EXECUTION error in a `200`,
    // never as a JSON-RPC error in a `500` (ruling R8): an HTTP MCP client reads a non-2xx as the
    // transport failing, so the model would learn nothing about which tool broke. The cause stays in
    // the node log, where the operator who can fix it reads it; the model is told only where to look.
    auto const internalError = [&] {
        return Reply { RenderToolResult(ToolResult { .structured = {},
                                                     .text = std::format("internal error in {}; see the node log", *name),
                                                     .isError = true },
                                        envelope.era) };
    };
    try
    {
        auto const result = tool->row.handler(ToolCall { .arguments = *arguments, .clientName = envelope.clientName });
        // A success with no structured content from a tool that PROMISED some (an `outputSchema`) is a
        // wrong answer that reads right to a client looking only at the text, and one a client
        // enforcing output schemas rejects (ruling R9). A tool ERROR owes no structured content.
        if (!result.isError && result.structured.IsNull() && tool->outputSchema)
        {
            _logger.Logf(LogLevel::Error,
                         "mcp: tool '{}' answered success without the structured content its output schema declares",
                         *name);
            return internalError();
        }
        return Reply { RenderToolResult(result, envelope.era) };
    }
    catch (std::exception const& error)
    {
        _logger.Logf(LogLevel::Error, "mcp: tool '{}' threw: {}", *name, error.what());
    }
    catch (...)
    {
        _logger.Logf(LogLevel::Error, "mcp: tool '{}' threw a non-standard exception", *name);
    }
    return internalError();
}

} // namespace Pace::Mcp
