// SPDX-License-Identifier: Apache-2.0
#include "RaftStoreArchiver.hpp"

#include <Pace/Cluster/FormationRecord.hpp>
#include <Pace/Consensus/FileRaftStorage.hpp>
#include <Pace/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/SecureRandomFakes.hpp>

using namespace Pace;
using namespace Pace::Node;
using Testing::ScratchDirectory;

namespace
{
/// Replace @p path's contents with @p text.
/// @param path The file.
/// @param text Its new contents.
void WriteText(std::filesystem::path const& path, std::string_view text)
{
    auto stream = std::ofstream { path, std::ios::binary | std::ios::trunc };
    stream << text;
}

/// The contents of a file, or empty when it cannot be read.
/// @param path The file.
/// @return Its contents.
[[nodiscard]] std::string ReadText(std::filesystem::path const& path)
{
    auto stream = std::ifstream { path, std::ios::binary | std::ios::ate };
    if (!stream.is_open())
        return {};
    auto const size = stream.tellg();
    if (size <= 0)
        return {};
    stream.seekg(0, std::ios::beg);
    auto text = std::string(static_cast<std::size_t>(size), '\0');
    stream.read(text.data(), size);
    return text;
}

/// Where @p name of cluster @p clusterId's store is archived under @p directory.
/// @param directory The state directory.
/// @param clusterId The cluster whose store it is.
/// @param name The store file.
/// @return Its archived path.
[[nodiscard]] std::filesystem::path ArchivedAt(std::filesystem::path const& directory,
                                               std::string_view clusterId,
                                               std::string_view name)
{
    return directory / ArchiveDirectoryName / std::filesystem::path { std::string { clusterId } } / name;
}
} // namespace

TEST_CASE("Archiving moves every store file under archive and a second run is a no-op", "[node][formation][archive]")
{
    ScratchDirectory const scratch { "archiver" };
    for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
        WriteText(scratch / name, name);
    WriteText(scratch / "node-key", "kept");
    WriteText(scratch / "formation", "kept");

    Consensus::SystemDurableFiles durableFiles;
    RaftStoreArchiver archiver { scratch.Path(), durableFiles };
    REQUIRE(archiver.Archive("c-laptop").has_value());
    for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
    {
        INFO(name);
        CHECK_FALSE(std::filesystem::exists(scratch / name));
        CHECK(ReadText(ArchivedAt(scratch.Path(), "c-laptop", name)) == name);
    }
    // The identity and the record are this MACHINE's, never a cluster's: they stay.
    CHECK(ReadText(scratch / "node-key") == "kept");
    CHECK(ReadText(scratch / "formation") == "kept");

    CHECK(archiver.Archive("c-laptop").has_value());
    for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
        CHECK(ReadText(ArchivedAt(scratch.Path(), "c-laptop", name)) == name);
}

TEST_CASE("An archive a crash interrupted halfway is finished", "[node][formation][archive]")
{
    ScratchDirectory const scratch { "archiver-half" };
    auto const names = Consensus::FileRaftStorage::StoreFileNames();
    REQUIRE(names.size() > 1);
    std::filesystem::create_directories(scratch / ArchiveDirectoryName / "c-laptop.partial");
    WriteText(ArchivedAt(scratch.Path(), "c-laptop.partial", names[0]), "moved already");
    for (auto const name: names.subspan(1))
        WriteText(scratch / name, name);

    Consensus::SystemDurableFiles durableFiles;
    RaftStoreArchiver archiver { scratch.Path(), durableFiles };
    REQUIRE(archiver.Archive("c-laptop").has_value());
    CHECK(ReadText(ArchivedAt(scratch.Path(), "c-laptop", names[0])) == "moved already");
    for (auto const name: names.subspan(1))
    {
        INFO(name);
        CHECK_FALSE(std::filesystem::exists(scratch / name));
        CHECK(ReadText(ArchivedAt(scratch.Path(), "c-laptop", name)) == name);
    }
    CHECK_FALSE(std::filesystem::exists(scratch / ArchiveDirectoryName / "c-laptop.partial"));
}

TEST_CASE("An archive a crash interrupted after the last move is renamed into place", "[node][formation][archive]")
{
    // Every file staged, the root empty, the rename not done: the next run has nothing to move and
    // still finishes, or a staging directory would be left to be mistaken for an archive in progress.
    ScratchDirectory const scratch { "archiver-staged" };
    std::filesystem::create_directories(scratch / ArchiveDirectoryName / "c-laptop.partial");
    for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
        WriteText(ArchivedAt(scratch.Path(), "c-laptop.partial", name), name);

    Consensus::SystemDurableFiles durableFiles;
    RaftStoreArchiver archiver { scratch.Path(), durableFiles };
    REQUIRE(archiver.Archive("c-laptop").has_value());
    for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
        CHECK(ReadText(ArchivedAt(scratch.Path(), "c-laptop", name)) == name);
    CHECK_FALSE(std::filesystem::exists(scratch / ArchiveDirectoryName / "c-laptop.partial"));
}

TEST_CASE("A cluster left twice is archived beside its first archive, never over it or into it",
          "[node][formation][archive]")
{
    ScratchDirectory const scratch { "archiver-twice" };
    Consensus::SystemDurableFiles durableFiles;
    RaftStoreArchiver archiver { scratch.Path(), durableFiles };
    for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
        WriteText(scratch / name, "first");
    REQUIRE(archiver.Archive("c-office").has_value());

    // Joined again, and left again: a store of its own in the root, under the same cluster id.
    auto const last = Consensus::FileRaftStorage::StoreFileNames().back();
    WriteText(scratch / last, "second");
    REQUIRE(archiver.Archive("c-office").has_value());

    for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
    {
        INFO(name);
        CHECK(ReadText(ArchivedAt(scratch.Path(), "c-office", name)) == "first"); // untouched
        CHECK_FALSE(std::filesystem::exists(scratch / name));
    }
    CHECK(ReadText(ArchivedAt(scratch.Path(), "c-office.1", last)) == "second");
    for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
        if (name != last)
            CHECK_FALSE(std::filesystem::exists(ArchivedAt(scratch.Path(), "c-office.1", name))); // nothing merged
}

TEST_CASE("A store file present in the root and in its archive is refused, and neither is written over",
          "[node][formation][archive]")
{
    ScratchDirectory const scratch { "archiver-both" };
    auto const name = Consensus::FileRaftStorage::StoreFileNames()[0];
    std::filesystem::create_directories(scratch / ArchiveDirectoryName / "c-laptop.partial");
    WriteText(ArchivedAt(scratch.Path(), "c-laptop.partial", name), "the archived one");
    WriteText(scratch / name, "the root one");

    Consensus::SystemDurableFiles durableFiles;
    RaftStoreArchiver archiver { scratch.Path(), durableFiles };
    auto const archived = archiver.Archive("c-laptop");
    REQUIRE_FALSE(archived.has_value());
    CHECK(archived.error().contains(std::string { name }));
    CHECK(archived.error().contains("c-laptop"));
    CHECK(ReadText(scratch / name) == "the root one");
    CHECK(ReadText(ArchivedAt(scratch.Path(), "c-laptop.partial", name)) == "the archived one");
}

TEST_CASE("An archive is named only by an id that is one safe path component", "[node][formation][archive]")
{
    // A fleet's id arrives in a remote summary and is about to become a directory: an allowlist, so
    // nothing that could leave the archive -- or name a device -- can be spelled.
    CHECK(IsArchivableClusterId("0123456789abcdef0123456789abcdef")); // what a mint produces
    CHECK(IsArchivableClusterId("c-office"));
    CHECK(IsArchivableClusterId(std::string(CompileCacheWire::MaxIdBytes, 'a')));

    for (auto const& refused: { std::string {},
                                std::string { "." },
                                std::string { ".." },
                                std::string { "a/b" },
                                std::string { "a\\b" },
                                std::string { "../escape" },
                                std::string { "C:" },
                                std::string { "a b" },
                                std::string { "CON" },
                                std::string { "Lpt1" },
                                std::string { "dotted.id" },
                                std::string { "caf\xc3\xa9" },
                                std::string(CompileCacheWire::MaxIdBytes + 1, 'a') })
    {
        CAPTURE(refused);
        CHECK_FALSE(IsArchivableClusterId(refused));
    }

    // The directories an archive may be named, and the ones it may not.
    for (auto const* const accepted: { "c-office", "c-office.1", "c-office.999", "c-office.partial" })
    {
        CAPTURE(accepted);
        CHECK(IsArchiveDirectoryName(accepted));
    }
    for (auto const* const refused:
         { "c-office.", "c-office.0", "c-office.01", "c-office.1000", "c-office.x", "a b.1", "c-office.1.1", ".partial" })
    {
        CAPTURE(refused);
        CHECK_FALSE(IsArchiveDirectoryName(refused));
    }

    // And the archiver asks it before touching anything.
    ScratchDirectory const scratch { "archiver-escape" };
    auto const name = Consensus::FileRaftStorage::StoreFileNames()[0];
    WriteText(scratch / name, "kept");
    Consensus::SystemDurableFiles durableFiles;
    RaftStoreArchiver archiver { scratch.Path(), durableFiles };
    REQUIRE_FALSE(archiver.Archive("../escape").has_value());
    CHECK(ReadText(scratch / name) == "kept");
    CHECK_FALSE(std::filesystem::exists(scratch / ArchiveDirectoryName));
}

TEST_CASE("Every cluster id the minter can draw is one an archive can be named by", "[node][formation][archive]")
{
    // Archive ids take no dot, so the rule is safe only while every id this build MINTS passes it.
    // Through the real minter, over every byte value -- the edges a hex rendering could get wrong
    // included -- rather than over a sample of plausible ids.
    auto script = std::vector<std::byte> {};
    for (auto const value: std::views::iota(0, 256))
        script.push_back(static_cast<std::byte>(value));
    auto random = Testing::ScriptedSecureRandom { script };
    auto minted = 0;
    for ([[maybe_unused]] auto const draw: std::views::iota(std::size_t { 0 }, script.size() / Cluster::ClusterIdBytes))
    {
        auto const id = Cluster::MintClusterId(random);
        REQUIRE(id.has_value());
        CAPTURE(*id);
        CHECK(IsArchivableClusterId(*id));
        ++minted;
    }
    CHECK(minted == 256 / static_cast<int>(Cluster::ClusterIdBytes)); // every byte value was drawn
}

namespace
{
/// This machine's files, recording every directory sync and answering each with @p answer.
class RecordingDirectorySyncs final: public Consensus::IDurableFiles
{
  public:
    explicit RecordingDirectorySyncs(std::error_code answer = {}) noexcept:
        _answer { answer }
    {
    }

    [[nodiscard]] std::expected<std::unique_ptr<Consensus::IDurableSink>, std::error_code> Create(
        std::filesystem::path const& path, StateFile which) override
    {
        return _system.Create(path, which);
    }

    [[nodiscard]] std::expected<void, Consensus::DirectorySyncFailure> SyncDirectory(
        std::filesystem::path const& directory) override
    {
        _synced.push_back(directory.lexically_normal());
        if (_answer)
            return std::unexpected { Consensus::DirectorySyncFailure { .step = Consensus::DirectorySyncStep::Flush,
                                                                       .code = _answer } };
        return {};
    }

    /// @return Every directory asked, in order.
    [[nodiscard]] std::vector<std::filesystem::path> const& Synced() const noexcept
    {
        return _synced;
    }

  private:
    std::vector<std::filesystem::path> _synced;
    std::error_code _answer;
    Consensus::SystemDurableFiles _system;
};
} // namespace

TEST_CASE("An archive syncs every directory its renames wrote, in order", "[node][formation][archive][1608]")
{
    // #1608: a rename reported before its directory reached the disk is one a power loss can take
    // back, and the archive is what a dissolve or a forget leaves. The root lost the files and the
    // staging directory gained them, BEFORE the rename that completes the archive; the archive
    // directory holds both names of that last rename.
    ScratchDirectory const scratch { "archiver-sync" };
    for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
        WriteText(scratch / name, name);
    RecordingDirectorySyncs files;
    RaftStoreArchiver archiver { scratch.Path(), files };
    REQUIRE(archiver.Archive("c-laptop").has_value());

    auto const archive = (scratch / ArchiveDirectoryName).lexically_normal();
    CHECK(files.Synced()
          == std::vector<std::filesystem::path> {
              scratch.Path().lexically_normal(), (archive / "c-laptop.partial").lexically_normal(), archive });
}

TEST_CASE("A directory sync that fails is a refusal, and one the volume cannot do is not",
          "[node][formation][archive][1608]")
{
    ScratchDirectory const scratch { "archiver-sync-fails" };
    for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
        WriteText(scratch / name, name);
    RecordingDirectorySyncs failing { std::make_error_code(std::errc::io_error) };
    auto const refused = RaftStoreArchiver { scratch.Path(), failing }.Archive("c-laptop");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("durable"));

    // The control: a volume that cannot sync a directory at all is degraded, as every state file's
    // replace is, and the archive completes.
    ScratchDirectory const other { "archiver-sync-unsupported" };
    for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
        WriteText(other / name, name);
    RecordingDirectorySyncs unsupported { Consensus::UnsupportedDirectorySyncAnswers().front() };
    CHECK(RaftStoreArchiver { other.Path(), unsupported }.Archive("c-laptop").has_value());
}
