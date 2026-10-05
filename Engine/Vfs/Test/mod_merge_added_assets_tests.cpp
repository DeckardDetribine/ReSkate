// Assets a mod ADDS to a bundle (new songs, say) while another mod ships its own copy of that
// bundle (a map does, for its levels). The merged bundle is built from one mod's copy, so
// without the carrying step the added assets are missing from it whenever the map's copy wins.
//
// The fixture is a small made-up game and two mods in a temp folder, written with the repo's
// own writers, so it needs no game files:
//   game   bundle "win32/test/shared": test/playlist, test/other
//   map    a level mod: its own level superbundle (a new TOC) with a copy of that bundle, as a
//          custom map carries the core assets for its level; it edits nothing
//   music  an asset mod that changes test/playlist to name a new song, and adds the song, the
//          wave the song names, the wave's sound-bank resource, and an asset nothing names.
#include "Engine/Resource/binary_bundle.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/toc.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/native_db.h"

#include <Windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace fb = dingosdk::frostbite;
namespace db = dingosdk::native_db;
using namespace dingosdk;

namespace {
using Bytes = std::vector<std::byte>;
constexpr std::uint32_t package = 0x1234;
constexpr char bundle_name[] = "win32/test/shared";

int failures = 0;
void expect(bool ok, const std::string& what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

// ---- Bytes ---------------------------------------------------------------------------------------
void put32(Bytes& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::byte>(value >> shift));
}
void put64(Bytes& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) out.push_back(static_cast<std::byte>(value >> shift));
}
void put_bytes(Bytes& out, std::string_view text) {
    for (const char c : text) out.push_back(static_cast<std::byte>(c));
}
void put_guid(Bytes& out, const fb::Guid& guid) { out.insert(out.end(), guid.bytes.begin(), guid.bytes.end()); }
fb::Guid guid(std::uint8_t seed) {
    fb::Guid value;
    value.bytes.fill(static_cast<std::byte>(seed));
    return value;
}
void write(const fs::path& path, std::span<const std::byte> bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
Bytes read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    Bytes bytes(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

// ---- An EBX document with no instances: a file GUID, the files it imports, and optional resource refs.
// That is all the merge reads from an asset to follow what it refers to.
void chunk(Bytes& out, const char (&id)[5], const Bytes& body) {
    put_bytes(out, std::string_view(id, 4));
    put32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    if (body.size() & 1) out.push_back(std::byte{0});
}
Bytes ebx_document(const fb::Guid& file, const std::vector<fb::Guid>& imports,
                   const Bytes& customData = {}, const std::vector<std::uint64_t>& resourceRefs = {}) {
    Bytes fixup;
    put_guid(fixup, file);
    for (int i = 0; i < 5; ++i) put32(fixup, 0);   // types, signatures, exported, instances, pointers
    put32(fixup, static_cast<std::uint32_t>(resourceRefs.size()));
    for (std::size_t i = 0; i < resourceRefs.size(); ++i) {
        put32(fixup, static_cast<std::uint32_t>(i * 8));
    }
    put32(fixup, static_cast<std::uint32_t>(imports.size()));
    for (const auto& import : imports) {
        put_guid(fixup, import);
        put_guid(fixup, guid(0xEE));               // the imported class
    }
    for (int i = 0; i < 2; ++i) put32(fixup, 0);   // import relocations, type-info references
    for (int i = 0; i < 3; ++i) put32(fixup, 0);   // array, boxed-value and string sections
    Bytes data;
    for (const auto ref : resourceRefs) put64(data, ref);
    data.insert(data.end(), customData.begin(), customData.end());
    if (data.size() < 16) data.resize(16);
    Bytes extra;
    put32(extra, 0);                               // arrays
    put32(extra, 0);                               // boxed values
    Bytes reflection;
    for (int i = 0; i < 5; ++i) put32(reflection, 0);   // signatures, types, fields, groups, mappings
    Bytes body;
    put_bytes(body, std::string_view("EBX\0", 4));
    chunk(body, "EFIX", fixup);
    chunk(body, "EBXD", data);
    chunk(body, "EBXX", extra);
    chunk(body, "REFL", reflection);
    Bytes riff;
    put_bytes(riff, "RIFF");
    put32(riff, static_cast<std::uint32_t>(body.size()));
    riff.insert(riff.end(), body.begin(), body.end());
    return riff;
}

// ---- The fixture's files -----------------------------------------------------------------------
// layout.toc with one install chunk, `package`, whose archives live in Win32/pkg.
Bytes layout_toc(const std::vector<std::string>& extra = {}) {
    const auto leaf = [](unsigned type, const char* name, std::vector<unsigned char> payload) {
        db::Node node;
        node.type = type;
        node.named = true;
        node.name = name;
        node.owned = std::move(payload);
        return node;
    };
    const auto container = [](unsigned type, const char* name, std::vector<db::Node> children) {
        db::Node node;
        node.type = type;
        node.named = *name != '\0';
        node.name = name;
        node.terminated = true;
        node.children = std::move(children);
        return node;
    };
    const auto records = [](const std::vector<std::string>& names) {
        std::vector<db::Node> rows;
        for (const auto& name : names) rows.push_back(db::make_named_record("name", name));
        return rows;
    };
    const auto low = static_cast<unsigned char>(package & 0xFF), high = static_cast<unsigned char>(package >> 8);
    const auto chunk_entry = container(2, "", {db::make_named_record("name", "pkg").children.front(),
                                              leaf(8, "persistentIndex", {low, high, 0, 0}),
                                              container(1, "superbundles", records(extra))});
    const auto root = container(2, "", {
        container(1, "superBundles", [&] { auto rows = records({"Win32/globals"}); for (auto& row : records(extra)) rows.push_back(std::move(row)); return rows; }()),
        container(2, "installManifest", {container(1, "installChunks", {chunk_entry})}),
        leaf(19, "layeredInstallChunkFiles", {1, 0, low, high, 0, 0, 0, 0})});
    const auto tree = db::write(root);
    Bytes file(db::envelope_size + tree.size());
    std::memcpy(file.data(), db::magic, sizeof(db::magic));
    std::memcpy(file.data() + db::envelope_size, tree.data(), tree.size());
    return file;
}

Bytes encoded(const Bytes& payload) { return fb::encode_cas(payload, {.compression = fb::CasCompression::raw}); }

fb::Sha1 sha(const std::string& name, unsigned version) {
    fb::Sha1 value;
    for (std::size_t i = 0; i < name.size(); ++i) value.bytes[i % 19] ^= static_cast<std::byte>(name[i]);
    value.bytes[19] = static_cast<std::byte>(version);
    return value;
}

struct Ebx { std::string name; unsigned version; Bytes payload; };
struct Resource {
    std::string name;
    Bytes payload;
    unsigned version = 0;
    std::uint64_t resourceId = 7;
    std::uint32_t resourceType = 0xb2c465f6;
    std::vector<std::byte> resourceMeta = std::vector<std::byte>(16, std::byte{0});
};

fb::BinaryBundle manifest_of(const std::vector<Ebx>& assets, const std::vector<Resource>& resources) {
    fb::BinaryBundle manifest;
    for (const auto& asset : assets) {
        fb::BundleAsset entry;
        entry.kind = fb::AssetKind::ebx;
        entry.name = asset.name;
        entry.sha1 = sha(asset.name, asset.version);
        entry.originalSize = asset.payload.size();
        manifest.ebx.push_back(std::move(entry));
    }
    for (const auto& resource : resources) {
        fb::BundleAsset entry;
        entry.kind = fb::AssetKind::resource;
        entry.name = resource.name;
        entry.sha1 = sha(resource.name, resource.version);
        entry.resourceId = resource.resourceId;
        entry.resourceType = resource.resourceType;
        entry.resourceMeta = resource.resourceMeta;
        entry.originalSize = resource.payload.size();
        manifest.resources.push_back(std::move(entry));
    }
    return manifest;
}

struct Fixture {
    fs::path root;
    mods::Catalog catalog;

    struct Chunk { fb::Guid guid; Bytes payload; };

    explicit Fixture(const std::string& name, const std::vector<Chunk>& baseChunks = {},
                     const std::vector<Resource>& baseResources = {}) {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        root = fs::path(temp) / ("reskate-merge-added-" + std::to_string(GetCurrentProcessId()) + "-" + name);
        fs::remove_all(root);
        catalog.data_root = root / "game";
        catalog.root = catalog.data_root / "Mods";
        catalog.present = true;
        // The game: two assets in one bundle, stored in the game's own archive.
        const std::vector<Ebx> assets{{"test/playlist", 0, ebx_document(guid(1), {})}, {"test/other", 0, ebx_document(guid(2), {})}};
        Bytes archive;
        std::vector<fb::BundleFileInfo> files;
        for (const auto& asset : assets) {
            const auto payload = encoded(asset.payload);
            files.push_back({{false, package, 1}, static_cast<std::uint32_t>(archive.size()), static_cast<std::uint32_t>(payload.size())});
            archive.insert(archive.end(), payload.begin(), payload.end());
        }
        for (const auto& resource : baseResources) {
            const auto payload = encoded(resource.payload);
            files.push_back({{false, package, 1}, static_cast<std::uint32_t>(archive.size()), static_cast<std::uint32_t>(payload.size())});
            archive.insert(archive.end(), payload.begin(), payload.end());
        }
        std::vector<fb::TocChunk> toc_chunks;
        for (const auto& chunk : baseChunks) {
            toc_chunks.push_back({chunk.guid, {false, package, 1}, static_cast<std::uint32_t>(archive.size()), static_cast<std::uint32_t>(chunk.payload.size())});
            archive.insert(archive.end(), chunk.payload.begin(), chunk.payload.end());
        }
        const std::vector<fb::TocBundle> bundles{{bundle_name, fb::write_bundle_region(files, fb::write_binary_bundle(manifest_of(assets, baseResources))), 1}};
        write(catalog.data_root / "Data" / "layout.toc", layout_toc());
        write(catalog.data_root / "Data" / "Win32" / "test_shared.toc", fb::write_patch_toc(bundles, toc_chunks));
        write(catalog.data_root / "Data" / "Win32" / "pkg" / "cas_01.cas", archive);
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    // A mod with its own copy of the bundle: the manifest first (raw), then every payload.
    void add(const std::string& name, bool levels, const std::string& toc, const std::vector<std::string>& superbundles,
             const std::vector<Ebx>& assets, const std::vector<Resource>& resources = {},
             const std::vector<Chunk>& chunks = {}, const std::vector<fb::TocChunk>& rawTocChunks = {}) {
        const auto listing = fb::write_binary_bundle(manifest_of(assets, resources));
        Bytes archive(listing);
        std::vector<fb::BundleFileInfo> files{{{true, package, 1}, 0, static_cast<std::uint32_t>(listing.size())}};
        const auto store = [&](const Bytes& payload) {
            const auto bytes = encoded(payload);
            files.push_back({{true, package, 1}, static_cast<std::uint32_t>(archive.size()), static_cast<std::uint32_t>(bytes.size())});
            archive.insert(archive.end(), bytes.begin(), bytes.end());
        };
        for (const auto& asset : assets) store(asset.payload);
        for (const auto& resource : resources) store(resource.payload);
        std::vector<fb::TocChunk> toc_chunks;
        for (const auto& chunk : chunks) {
            toc_chunks.push_back({chunk.guid, {true, package, 1}, static_cast<std::uint32_t>(archive.size()), static_cast<std::uint32_t>(chunk.payload.size())});
            archive.insert(archive.end(), chunk.payload.begin(), chunk.payload.end());
        }
        for (const auto& raw : rawTocChunks) toc_chunks.push_back(raw);
        const std::vector<fb::TocBundle> bundles{{bundle_name, fb::write_bundle_region(files), 1}};
        const auto directory = catalog.root / name;
        write(directory / "layout.toc", layout_toc(superbundles));
        write(directory / "Win32" / fs::path(toc), fb::write_patch_toc(bundles, toc_chunks));
        write(directory / "Win32" / "pkg" / "cas_01.cas", archive);
        mods::Mod mod;
        mod.name = name;
        mod.directory = directory;
        mod.provides_layout = true;
        mod.provides_levels = levels;
        catalog.mods.push_back(std::move(mod));
    }

    // How many files the merged copy of the bundle in `toc` lists, or -1 when the merge left none.
    int merged_files(const std::string& relative) const {
        const auto toc = catalog.root / mods::generated_folder / "Win32" / fs::path(relative);
        if (!fs::exists(toc)) return -1;
        for (const auto& bundle : fb::read_toc(read(toc)).bundles)
            if (bundle.name == bundle_name) return static_cast<int>(fb::read_bundle_region(bundle.region).files.size());
        return -1;
    }

    std::optional<std::pair<fb::TocChunk, Bytes>> merged_chunk(const std::string& relative, const fb::Guid& guid) const {
        const auto tocPath = catalog.root / mods::generated_folder / "Win32" / fs::path(relative);
        if (!fs::exists(tocPath)) return std::nullopt;
        const auto doc = fb::read_toc(read(tocPath));
        for (const auto& chunk : doc.chunks) {
            if (chunk.guid == guid) {
                const auto archiveFile = "cas_" + (chunk.location.archive < 10 ? std::string("0") : std::string{}) +
                                         std::to_string(chunk.location.archive) + ".cas";
                const auto archivePath = chunk.location.patch
                    ? catalog.root / mods::generated_folder / "Win32" / "pkg" / fs::path(archiveFile)
                    : catalog.data_root / "Data" / "Win32" / "pkg" / fs::path(archiveFile);
                if (!fs::exists(archivePath)) return std::make_pair(chunk, Bytes{});
                const auto archive = read(archivePath);
                if (chunk.offset + chunk.size > archive.size()) return std::make_pair(chunk, Bytes{});
                Bytes payload(archive.begin() + chunk.offset, archive.begin() + chunk.offset + chunk.size);
                return std::make_pair(chunk, std::move(payload));
            }
        }
        return std::nullopt;
    }

    struct MergedAsset {
        fb::BundleAsset asset;
        Bytes payload;
    };
    std::optional<MergedAsset> merged_asset(const std::string& relative, fb::AssetKind kind, const std::string& name,
                                            const std::string& targetBundle = bundle_name) const {
        const auto tocPath = catalog.root / mods::generated_folder / "Win32" / fs::path(relative);
        if (!fs::exists(tocPath)) return std::nullopt;
        const auto doc = fb::read_toc(read(tocPath));
        for (const auto& bundle : doc.bundles) {
            if (bundle.name != targetBundle) continue;
            const auto region = fb::read_bundle_region(bundle.region);
            fb::BinaryBundle manifest;
            std::size_t fileOffset = 0;
            if (!region.inlineManifest.empty()) {
                manifest = fb::read_binary_bundle(region.inlineManifest);
            } else if (!region.files.empty()) {
                const auto& mfile = region.files.front();
                const auto archiveFile = "cas_" + (mfile.location.archive < 10 ? std::string("0") : std::string{}) +
                                         std::to_string(mfile.location.archive) + ".cas";
                const auto archivePath = mfile.location.patch
                    ? catalog.root / mods::generated_folder / "Win32" / "pkg" / fs::path(archiveFile)
                    : catalog.data_root / "Data" / "Win32" / "pkg" / fs::path(archiveFile);
                if (!fs::exists(archivePath)) return std::nullopt;
                const auto archive = read(archivePath);
                if (mfile.offset + mfile.size > archive.size()) return std::nullopt;
                manifest = fb::read_binary_bundle(std::span<const std::byte>(archive.data() + mfile.offset, mfile.size));
                fileOffset = 1;
            } else {
                return std::nullopt;
            }

            const auto& list = (kind == fb::AssetKind::ebx) ? manifest.ebx : manifest.resources;
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (list[i].name == name) {
                    const std::size_t fileIdx = fileOffset + (kind == fb::AssetKind::ebx ? 0 : manifest.ebx.size()) + i;
                    if (fileIdx >= region.files.size()) return std::nullopt;
                    const auto& file = region.files[fileIdx];
                    const auto archiveFile = "cas_" + (file.location.archive < 10 ? std::string("0") : std::string{}) +
                                             std::to_string(file.location.archive) + ".cas";
                    const auto archivePath = file.location.patch
                        ? catalog.root / mods::generated_folder / "Win32" / "pkg" / fs::path(archiveFile)
                        : catalog.data_root / "Data" / "Win32" / "pkg" / fs::path(archiveFile);
                    if (!fs::exists(archivePath)) return std::nullopt;
                    const auto archive = read(archivePath);
                    if (file.offset + file.size > archive.size()) return std::nullopt;
                    Bytes raw(archive.begin() + file.offset, archive.begin() + file.offset + file.size);
                    Bytes decodedPayload;
                    try {
                        decodedPayload = fb::decode_cas(raw, {});
                    } catch (...) {
                        decodedPayload = std::move(raw);
                    }
                    return MergedAsset{list[i], std::move(decodedPayload)};
                }
            }
        }
        return std::nullopt;
    }
};

// The game's copy of the bundle, as a level mod carries it.
std::vector<Ebx> game_copy() {
    return {{"test/playlist", 0, ebx_document(guid(1), {})}, {"test/other", 0, ebx_document(guid(2), {})}};
}
// The music mod's bundle: the playlist now names the song (a change), the song names its wave,
// and one more asset nothing names.
std::vector<Ebx> music_assets() {
    return {{"test/playlist", 1, ebx_document(guid(1), {guid(10)})},
            {"test/other", 0, ebx_document(guid(2), {})},
            {"test/song", 0, ebx_document(guid(10), {guid(11)})},
            {"test/wave", 0, ebx_document(guid(11), {})},
            {"test/unrelated", 0, ebx_document(guid(12), {})}};
}
std::vector<Resource> music_resources() { return {{"test/wave", {std::byte{1}, std::byte{2}, std::byte{3}}}}; }

bool noted(const mods::MergeReport& report, const std::string& text) {
    for (const auto& note : report.notes)
        if (note == text) return true;
    return false;
}
std::string describe(const mods::MergeReport& report) {
    std::string text = "issue: '" + report.issue + "'";
    for (const auto& note : report.notes) text += "\n  note: " + note;
    for (const auto& [mod, problems] : report.problems)
        for (const auto& problem : problems) text += "\n  " + mod + ": " + problem;
    return text;
}

constexpr char shared_toc[] = "test_shared.toc";
constexpr char map_toc[] = "levels/test/map.toc";
constexpr char map_superbundle[] = "Win32/levels/test/map";

// The level mod alone: what its copy of the bundle holds with nothing carried into it.
int map_only() {
    Fixture fixture("map-only");
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "map only: the merge builds the patch\n" + describe(report));
    return fixture.merged_files(map_toc);
}

// The music mod edits the game's TOC; the map's copy of the bundle is in its own TOC, so nothing
// of the music mod reaches it unless the merge carries it. The playlist change always did (it is a
// change to an asset the map's copy has); the song, its wave and the wave's resource are what
// the changed playlist leads to. The asset nothing names must stay out. Either mod may come first.
void carried_into_the_maps_copy(bool map_first, int baseline) {
    const std::string order = map_first ? "map first" : "music first";
    Fixture fixture(map_first ? "map-first" : "music-first");
    const auto add_map = [&] { fixture.add("map", true, map_toc, {map_superbundle}, game_copy()); };
    const auto add_music = [&] { fixture.add("music", false, shared_toc, {}, music_assets(), music_resources()); };
    if (map_first) { add_map(); add_music(); } else { add_music(); add_map(); }
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, order + ": the merge builds the patch\n" + describe(report));
    expect(noted(report, std::string("map: ") + bundle_name + ": 3 asset(s) added by other mods, e.g. test/song"),
           order + ": the map's copy receives them\n" + describe(report));
    const auto files = fixture.merged_files(map_toc);
    expect(files == baseline + 3, order + ": the map's copy gained those three files and not the unnamed asset (" +
           std::to_string(files) + " files, " + std::to_string(baseline) + " without the music mod)");
}

// Another asset mod ships the game's copy of the bundle in the TOC the music mod ships, the way
// cosmetic mods share one. That copy merges with the music mod's bundle, which already has the
// song, so nothing is carried into it (no "added by other mods" note), and the merged bundle is
// exactly the union: the manifest, the music mod's five EBX and its resource.
void copy_in_the_adders_toc_is_left_alone() {
    Fixture fixture("same-toc");
    fixture.add("music", false, shared_toc, {}, music_assets(), music_resources());
    fixture.add("other", false, shared_toc, {}, game_copy());
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "same TOC: the merge builds the patch\n" + describe(report));
    for (const auto& note : report.notes)
        expect(note.find("added by other mods") == std::string::npos,
               "same TOC: nothing is carried into a copy in the adder's own TOC: " + note);
    const auto expected = static_cast<int>(1 + music_assets().size() + music_resources().size());
    const auto files = fixture.merged_files(shared_toc);
    expect(files == expected, "same TOC: the merged bundle is the union, " + std::to_string(expected) + " files (" +
           std::to_string(files) + " files)");
}
// Two asset mods each add test/song, each named by a change of its own (the rival's is to the
// other asset). Only one can be carried into the map's copy: the higher-priority mod's. When the
// two are different assets the merge says whose the name went to; the same asset added by both
// is no clash and is not reported. The bundle the two mods share keeps one asset per name as
// well, and says so the same way.
void same_name_from_two_mods(bool different) {
    const std::string label = different ? "name clash" : "same asset twice";
    Fixture fixture(different ? "name-clash" : "same-twice");
    const auto song = guid(different ? 20 : 10);
    fixture.add("music", false, shared_toc, {}, music_assets(), music_resources());
    fixture.add("rival", false, shared_toc, {},
                {{"test/playlist", 0, ebx_document(guid(1), {})},
                 {"test/other", 1, ebx_document(guid(2), {song})},
                 {"test/song", different ? 1u : 0u, ebx_document(song, {})}});
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, label + ": the merge builds the patch\n" + describe(report));
    const bool said = noted(report, "rival: 1 added asset(s) share a name with ones music adds, e.g. test/song; "
                                    "other mods' copies of the bundle get music's");
    expect(said == different, label + (different ? ": the merge says whose asset the name went to\n"
                                                 : ": nothing is reported\n") + describe(report));
    const bool kept = noted(report, std::string("rival: ") + bundle_name + ": 1 added asset(s) share a name with ones "
                                    "music adds, e.g. test/song; the merged bundle keeps music's");
    expect(kept == different, label + (different ? ": the merge says whose asset the shared bundle keeps\n"
                                                 : ": nothing is reported about the shared bundle\n") + describe(report));
    expect(noted(report, std::string("map: ") + bundle_name + ": 3 asset(s) added by other mods, e.g. test/song"),
           label + ": the map's copy still receives the first mod's three\n" + describe(report));
}

// An asset mod changes an existing EBX (test/playlist) to reference a new TOC chunk,
// adding NO new EBX or resources. The map carries the game's copy of test/playlist.
// The merged map must receive the changed EBX and its referenced chunk, and reading
// the chunk from the merged archive must return the donor's payload.
void changed_ebx_carries_its_chunks_into_maps_copy() {
    Fixture fixture("changed-ebx-chunk");
    const auto chunkGuid = guid(0x77);
    const Bytes chunkPayload{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    Bytes ebxData;
    put_guid(ebxData, chunkGuid);
    fixture.add("patchmod", false, shared_toc, {},
                {{"test/playlist", 1, ebx_document(guid(1), {}, ebxData)},
                 {"test/other", 0, ebx_document(guid(2), {})}},
                {},
                {{chunkGuid, chunkPayload}});
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());
    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "changed EBX chunk: merge builds patch\n" + describe(report));
    const auto chunkResult = fixture.merged_chunk(map_toc, chunkGuid);
    expect(chunkResult.has_value(), "changed EBX chunk: map superbundle TOC registers the new chunk");
    if (chunkResult) {
        expect(chunkResult->second == chunkPayload, "changed EBX chunk: reading chunk returns donor payload");
    }
}

// An existing chunk GUID present in the base game has its payload modified by an asset mod.
// A map superbundle carrying the base chunk or base bundle must receive the mod's
// modified chunk payload rather than keeping the old base chunk.
void changed_chunk_retaining_existing_guid_propagates() {
    const auto chunkGuid = guid(0x42);
    const Bytes basePayload{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    const Bytes modPayload{std::byte{0x99}, std::byte{0x88}, std::byte{0x77}, std::byte{0x66}};

    Fixture fixture("changed-chunk-existing-guid", {{chunkGuid, basePayload}});

    Bytes ebxData;
    put_guid(ebxData, chunkGuid);
    fixture.add("patchmod", false, shared_toc, {},
                {{"test/playlist", 1, ebx_document(guid(1), {}, ebxData)},
                 {"test/other", 0, ebx_document(guid(2), {})}},
                {},
                {{chunkGuid, modPayload}});
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());

    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "changed existing chunk: merge builds patch\n" + describe(report));
    const auto chunkResult = fixture.merged_chunk(map_toc, chunkGuid);
    expect(chunkResult.has_value(), "changed existing chunk: map superbundle TOC registers the chunk");
    if (chunkResult) {
        expect(chunkResult->second == modPayload, "changed existing chunk: reading chunk returns modified mod payload");
    }
}

// An asset mod replaces an existing resource in a base bundle with new payload and updated metadata.
// A map superbundle carrying the base bundle must receive the mod's replaced resource payload and metadata.
void changed_resource_replaces_base_resource_in_maps_copy() {
    const std::string resName = "test/collision";
    const Bytes baseResPayload{std::byte{0x55}, std::byte{0x66}, std::byte{0x77}};
    const Bytes modResPayload{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD}};

    Resource baseRes;
    baseRes.name = resName;
    baseRes.payload = baseResPayload;
    baseRes.version = 0;
    baseRes.resourceType = 0x11112222;

    Fixture fixture("changed-resource-replacement", {}, {baseRes});

    Resource modRes;
    modRes.name = resName;
    modRes.payload = modResPayload;
    modRes.version = 1;
    modRes.resourceType = 0x33334444;

    fixture.add("patchmod", false, shared_toc, {},
                {{"test/playlist", 0, ebx_document(guid(1), {})},
                 {"test/other", 0, ebx_document(guid(2), {})}},
                {modRes});
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy(), {baseRes});

    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "changed resource: merge builds patch\n" + describe(report));
    const auto assetResult = fixture.merged_asset(map_toc, fb::AssetKind::resource, resName);
    expect(assetResult.has_value(), "changed resource: map superbundle bundle contains the resource");
    if (assetResult) {
        expect(assetResult->payload == modResPayload, "changed resource: reading resource returns modified mod payload");
        expect(assetResult->asset.sha1 == sha(resName, 1), "changed resource: resource SHA1 matches mod replacement");
        expect(assetResult->asset.resourceType == 0x33334444, "changed resource: resourceType matches mod replacement");
    }
}

// Transitive dependency resolution:
// - A changed EBX ("test/playlist") imports an added EBX ("test/song").
// - Added EBX ("test/song") imports an added EBX ("test/wave").
// - Added EBX ("test/wave") has a resource reference pointing to resourceId 0x12345678.
// - An added resource ("res/audio_stream", different name!) has resourceId 0x12345678,
//   and its payload contains the GUID of chunk 0x88.
// - Mod also adds chunk 0x88.
// - Mod also adds unrelated EBX, resource, and chunk which must NOT be taken.
// When map's copy is merged, it must receive:
// test/song, test/wave, res/audio_stream, and chunk 0x88.
void transitive_structured_dependencies_follow_selected_ebx() {
    // Non-level TOCs retain selective dependency forwarding.
    constexpr char selective_toc[] = "test_map.toc";
    constexpr char selective_superbundle[] = "Win32/test_map";
    Fixture fixture("transitive-deps");

    const auto songGuid = guid(0xA1);
    const auto waveGuid = guid(0xA2);
    const auto unrelatedGuid = guid(0x99);
    const auto chunkGuid = guid(0x88);
    const auto unrelatedChunkGuid = guid(0x77);

    const std::uint64_t audioStreamResId = 0x12345678ULL;
    const std::uint64_t unrelatedResId = 0x99999999ULL;

    Bytes resPayload;
    put_guid(resPayload, chunkGuid);
    resPayload.resize(32, std::byte{0x55});

    Bytes unrelatedResPayload;
    put_guid(unrelatedResPayload, unrelatedChunkGuid);

    Resource audioStreamRes;
    audioStreamRes.name = "res/audio_stream";
    audioStreamRes.payload = resPayload;
    audioStreamRes.resourceId = audioStreamResId;

    Resource unrelatedRes;
    unrelatedRes.name = "res/unrelated";
    unrelatedRes.payload = unrelatedResPayload;
    unrelatedRes.resourceId = unrelatedResId;

    const Bytes chunkPayload{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    const Bytes unrelatedChunkPayload{std::byte{0x01}, std::byte{0x02}};

    fixture.add("patchmod", false, shared_toc, {},
                {{"test/playlist", 1, ebx_document(guid(1), {songGuid})},
                 {"test/other", 0, ebx_document(guid(2), {})},
                 {"test/song", 0, ebx_document(songGuid, {waveGuid})},
                 {"test/wave", 0, ebx_document(waveGuid, {}, {}, {audioStreamResId})},
                 {"test/unrelated", 0, ebx_document(unrelatedGuid, {}, {}, {unrelatedResId})}},
                {audioStreamRes, unrelatedRes},
                {{chunkGuid, chunkPayload}, {unrelatedChunkGuid, unrelatedChunkPayload}});

    fixture.add("map", true, selective_toc, {selective_superbundle}, game_copy());

    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "transitive deps: merge builds patch\n" + describe(report));

    const auto song = fixture.merged_asset(selective_toc, fb::AssetKind::ebx, "test/song");
    expect(song.has_value(), "transitive deps: map copy receives imported test/song");

    const auto wave = fixture.merged_asset(selective_toc, fb::AssetKind::ebx, "test/wave");
    expect(wave.has_value(), "transitive deps: map copy receives transitively imported test/wave");

    const auto stream = fixture.merged_asset(selective_toc, fb::AssetKind::resource, "res/audio_stream");
    expect(stream.has_value(), "transitive deps: map copy receives referenced res/audio_stream");

    const auto chunk = fixture.merged_chunk(selective_toc, chunkGuid);
    expect(chunk.has_value(), "transitive deps: map copy receives chunk 0x88 from resource");
    if (chunk) {
        expect(chunk->second == chunkPayload, "transitive deps: chunk 0x88 payload matches");
    }

    const auto unrelatedEbx = fixture.merged_asset(selective_toc, fb::AssetKind::ebx, "test/unrelated");
    expect(!unrelatedEbx.has_value(), "transitive deps: unreferenced test/unrelated is NOT merged");

    const auto unrelatedResource = fixture.merged_asset(selective_toc, fb::AssetKind::resource, "res/unrelated");
    expect(!unrelatedResource.has_value(), "transitive deps: unreferenced res/unrelated is NOT merged");

    const auto unrelatedChunk = fixture.merged_chunk(selective_toc, unrelatedChunkGuid);
    expect(!unrelatedChunk.has_value(), "transitive deps: unreferenced chunk is NOT merged");
}

void transitive_ebx_import_cycle_terminates_and_merges_all_cycle_members() {
    Fixture fixture("cycle-deps");

    const auto nodeA = guid(0x51);
    const auto nodeB = guid(0x52);

    fixture.add("patchmod", false, shared_toc, {},
                {{"test/playlist", 1, ebx_document(guid(1), {nodeA})},
                 {"test/other", 0, ebx_document(guid(2), {})},
                 {"test/nodeA", 0, ebx_document(nodeA, {nodeB})},
                 {"test/nodeB", 0, ebx_document(nodeB, {nodeA})}});

    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());

    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.issue.empty() && report.built, "cycle deps: merge builds patch\n" + describe(report));

    const auto a = fixture.merged_asset(map_toc, fb::AssetKind::ebx, "test/nodeA");
    expect(a.has_value(), "cycle deps: map copy receives nodeA");

    const auto b = fixture.merged_asset(map_toc, fb::AssetKind::ebx, "test/nodeB");
    expect(b.has_value(), "cycle deps: map copy receives cyclic nodeB");
}

// If a mod changes an existing EBX to reference a chunk, but that chunk is unreadable
// (corrupted, truncated, or invalid archive offset), the merger must not publish a broken asset.
// Instead, the destination must retain the prior complete version (the game's original copy),
// the unreadable chunk must not be registered in the destination TOC, and an explanatory note
// must be recorded.
void unreadable_chunk_dependency_retains_prior_asset_version() {
    Fixture fixture("unreadable-chunk");

    const auto chunkGuid = guid(0x99);
    Bytes ebxData;
    put_guid(ebxData, chunkGuid);

    // Broken chunk pointing to byte 0x900000 in cas_01, far past the end of the file.
    fb::TocChunk brokenChunk{chunkGuid, {true, package, 1}, 0x900000, 0x100, false};

    fixture.add("patchmod", false, shared_toc, {},
                {{"test/playlist", 1, ebx_document(guid(1), {}, ebxData)},
                 {"test/other", 0, ebx_document(guid(2), {})}},
                {},
                {},
                {brokenChunk});

    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());

    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.built, "unreadable chunk: merge builds patch\n" + describe(report));

    const auto assetResult = fixture.merged_asset(map_toc, fb::AssetKind::ebx, "test/playlist");
    expect(assetResult.has_value(), "unreadable chunk: destination has test/playlist");
    if (assetResult) {
        expect(assetResult->asset.sha1 == sha("test/playlist", 0),
               "unreadable chunk: destination retains base game copy (version 0)");
    }

    const auto chunkResult = fixture.merged_chunk(map_toc, chunkGuid);
    expect(!chunkResult.has_value(), "unreadable chunk: unreadable chunk is NOT registered in map TOC");

    const auto noteFound = std::ranges::any_of(report.notes, [](const std::string& note) {
        return note.find("test/playlist") != std::string::npos &&
               (note.find("kept the game's copy") != std::string::npos || note.find("unreadable") != std::string::npos);
    });
    expect(noteFound, "unreadable chunk: explanatory note is recorded");
}

void unreadable_chunk_dependency_aborts_asset_addition() {
    Fixture fixture("unreadable-chunk-add");

    const auto songGuid = guid(0x55);
    const auto chunkGuid = guid(0x66);
    Bytes songData;
    put_guid(songData, chunkGuid);

    fb::TocChunk brokenChunk{chunkGuid, {true, package, 1}, 0x900000, 0x100, false};

    fixture.add("patchmod", false, shared_toc, {},
                {{"test/playlist", 1, ebx_document(guid(1), {songGuid})},
                 {"test/other", 0, ebx_document(guid(2), {})},
                 {"test/song", 0, ebx_document(songGuid, {}, songData)}},
                {},
                {},
                {brokenChunk});

    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());

    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.built, "unreadable chunk add: merge builds patch\n" + describe(report));

    const auto songResult = fixture.merged_asset(map_toc, fb::AssetKind::ebx, "test/song");
    expect(!songResult.has_value(), "unreadable chunk add: test/song is NOT added to destination");

    const auto chunkResult = fixture.merged_chunk(map_toc, chunkGuid);
    expect(!chunkResult.has_value(), "unreadable chunk add: broken chunk is NOT in map TOC");
}

void bounded_diagnostics_report_merge_decisions() {
    const auto existingChunkGuid = guid(0x11);
    const Bytes baseChunkPayload{std::byte{0x10}, std::byte{0x20}};
    const Bytes modChunkPayload{std::byte{0x99}, std::byte{0x88}};

    const auto newChunkGuid = guid(0x33);
    const Bytes newChunkPayload{std::byte{0x55}, std::byte{0x66}};

    Fixture fixture("diagnostics-decisions",
                    {{existingChunkGuid, baseChunkPayload}});

    Bytes playlistData;
    put_guid(playlistData, existingChunkGuid);
    put_guid(playlistData, newChunkGuid);

    Bytes otherData;
    put_guid(otherData, newChunkGuid);

    // patchmod modifies playlist and other:
    // - existingChunkGuid with modChunkPayload (replaced)
    // - newChunkGuid with newChunkPayload (first copied for playlist, then already satisfied for other)
    fixture.add("patchmod", false, shared_toc, {},
                {{"test/playlist", 1, ebx_document(guid(1), {}, playlistData)},
                 {"test/other", 1, ebx_document(guid(2), {}, otherData)}},
                {},
                {{existingChunkGuid, modChunkPayload},
                 {newChunkGuid, newChunkPayload}});

    // map carries the base chunks
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy(), {},
                {{existingChunkGuid, baseChunkPayload}});

    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.built, "diagnostics: merge builds patch\n" + describe(report));

    // Verify "replaced" note for modified chunk
    const auto replacedNoted = std::ranges::any_of(report.notes, [](const std::string& note) {
        return note.find("replaced") != std::string::npos && note.find("chunk(s)") != std::string::npos;
    });
    expect(replacedNoted, "diagnostics: records chunk replaced note\n" + describe(report));

    // Verify "already satisfied" note for identical chunk
    const auto satisfiedNoted = std::ranges::any_of(report.notes, [](const std::string& note) {
        return note.find("already satisfied") != std::string::npos;
    });
    expect(satisfiedNoted, "diagnostics: records chunk already satisfied note\n" + describe(report));

    // Verify "copied" / new chunk note
    const auto copiedNoted = std::ranges::any_of(report.notes, [](const std::string& note) {
        return note.find("copied") != std::string::npos && note.find("chunk(s)") != std::string::npos;
    });
    expect(copiedNoted, "diagnostics: records chunk copied note\n" + describe(report));

    // Verify output integrity: replaced chunk reads mod payload from generated patch archive
    const auto replacedChunk = fixture.merged_chunk(map_toc, existingChunkGuid);
    expect(replacedChunk.has_value(), "diagnostics: replaced chunk is in map TOC");
    if (replacedChunk) {
        expect(replacedChunk->second == modChunkPayload, "diagnostics: replaced chunk matches donor payload");
    }

    // Verify output integrity: copied chunk reads new chunk payload from generated patch archive
    const auto copiedChunk = fixture.merged_chunk(map_toc, newChunkGuid);
    expect(copiedChunk.has_value(), "diagnostics: copied chunk is in map TOC");
    if (copiedChunk) {
        expect(copiedChunk->second == newChunkPayload, "diagnostics: copied chunk matches donor payload");
    }

    // Verify output integrity: destination EBX was updated and readable
    const auto playlistAsset = fixture.merged_asset(map_toc, fb::AssetKind::ebx, "test/playlist");
    expect(playlistAsset.has_value(), "diagnostics: updated test/playlist is readable from bundle");
}

void level_without_matching_bundles_receives_asset_mod_chunks() {
    Fixture fixture("level-independent-chunks");
    const auto chunkGuid = guid(0xD1);
    const Bytes payload{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
    const Bytes lowerPriority{std::byte{0x44}};
    // Neither donor changes an asset or adds a dependency to a bundle.
    fixture.add("cosmetic", false, shared_toc, {}, game_copy(), {}, {{chunkGuid, payload}});
    fixture.add("other-cosmetic", false, shared_toc, {}, game_copy(), {}, {{chunkGuid, lowerPriority}});
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());
    const auto mapPath = fixture.catalog.mods.back().directory / "Win32" / map_toc;
    auto level = fb::read_toc(read(mapPath));
    level.bundles.front().name = "win32/test/level_only";
    write(mapPath, fb::write_patch_toc(level.bundles, level.chunks));

    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.built && report.issue.empty(), "level chunks: merge succeeds\n" + describe(report));
    const auto chunk = fixture.merged_chunk(map_toc, chunkGuid);
    expect(chunk.has_value(), "level chunks: TOC without matching bundles receives cosmetic chunk");
    if (chunk) expect(chunk->second == payload, "level chunks: shifted archive reads highest-priority payload");
    const auto merged = fb::read_toc(read(fixture.catalog.root / mods::generated_folder / "Win32" / map_toc));
    expect(std::ranges::count_if(merged.chunks, [&](const auto& c) { return c.guid == chunkGuid; }) == 1,
           "level chunks: duplicate GUID is published once");
}

void changed_asset_does_not_override_different_bundle() {
    Fixture fixture("changed-different-bundle");
    // Mod A changes test/playlist in win32/test/shared.
    fixture.add("patchmod", false, shared_toc, {},
                {{"test/playlist", 1, ebx_document(guid(1), {})},
                 {"test/other", 0, ebx_document(guid(2), {})}});
    // Map has a custom bundle "win32/test/level_only" containing test/playlist with version 0.
    fixture.add("map", true, map_toc, {map_superbundle}, game_copy());
    const auto mapPath = fixture.catalog.mods.back().directory / "Win32" / map_toc;
    auto level = fb::read_toc(read(mapPath));
    level.bundles.front().name = "win32/test/level_only";
    write(mapPath, fb::write_patch_toc(level.bundles, level.chunks));

    const auto report = mods::merge_mods(fixture.catalog);
    expect(report.built && report.issue.empty(), "different bundle: merge succeeds\n" + describe(report));
    const auto asset = fixture.merged_asset(map_toc, fb::AssetKind::ebx, "test/playlist", "win32/test/level_only");
    expect(asset.has_value(), "different bundle: level bundle contains test/playlist");
    if (asset) {
        expect(asset->asset.sha1 == sha("test/playlist", 0),
               "different bundle: asset in distinct bundle is NOT overwritten by foreign bundle change");
    }
}
} // namespace

int main() try {
    const auto baseline = map_only();
    expect(baseline > 0, "map only: the map's copy exists (" + std::to_string(baseline) + ")");
    carried_into_the_maps_copy(true, baseline);
    carried_into_the_maps_copy(false, baseline);
    copy_in_the_adders_toc_is_left_alone();
    same_name_from_two_mods(true);
    same_name_from_two_mods(false);
    changed_ebx_carries_its_chunks_into_maps_copy();
    changed_chunk_retaining_existing_guid_propagates();
    changed_resource_replaces_base_resource_in_maps_copy();
    changed_asset_does_not_override_different_bundle();
    transitive_structured_dependencies_follow_selected_ebx();
    transitive_ebx_import_cycle_terminates_and_merges_all_cycle_members();
    unreadable_chunk_dependency_retains_prior_asset_version();
    unreadable_chunk_dependency_aborts_asset_addition();
    bounded_diagnostics_report_merge_decisions();
    level_without_matching_bundles_receives_asset_mod_chunks();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "mod merge added assets: ok\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
