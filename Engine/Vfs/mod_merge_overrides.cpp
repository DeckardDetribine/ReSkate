#include "mod_merge_internal.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Resource/ebx_document.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace dingosdk::mods::detail {

AssetOverrides collect_asset_overrides(const std::vector<const Mod*>& mods,
                                       const std::map<const Mod*, RelativeFiles>& modFiles,
                                       const CasStore& store, const fs::path& baseRoot,
                                       const fs::path& gameRoot, MergeReport& report) {
    AssetOverrides out;
    // Highest priority first: an asset a higher mod already changed keeps that change.
    for (const auto* mod : mods) {
        // A map's edits to the game's assets serve its own levels; asset mods
        // (cameras, tuning, cosmetics) are the ones meant to apply everywhere.
        if (mod->provides_levels) continue;
        const auto files = modFiles.find(mod);
        if (files == modFiles.end()) continue;
        std::size_t changed{};
        // This mod's new assets, and the partitions its recorded changes import.
        // An added asset only follows a change that refers to it: copied on its
        // own, it could arrive in another mod's bundle without the resources
        // and chunks it was added with.
        std::vector<std::pair<std::string, AssetAddition>> candidates, companions;
        std::vector<fb::TocChunk> newChunks;   // TOC chunks the game's TOCs do not have
        std::set<fb::Guid> imported;
        std::set<std::uint64_t> referencedResources;
        struct EbxReferences {
            fb::Guid fileGuid;
            std::vector<fb::Guid> imports;
            std::vector<std::uint64_t> resourceRefs;
        };
        const auto parse_ebx = [&](std::span<const std::byte> encoded) -> EbxReferences {
            const auto bytes = fb::decode_cas(encoded, {gameRoot});
            const auto doc = fb::ebx::read_document(bytes);
            EbxReferences refs;
            refs.fileGuid = doc.fileGuid;
            refs.imports.reserve(doc.imports.size());
            for (const auto& imp : doc.imports) refs.imports.push_back(imp.fileGuid);
            refs.resourceRefs.reserve(doc.resourceRefOffsets.size());
            for (const auto offset : doc.resourceRefOffsets) {
                if (doc.dataStart + offset + 8 <= bytes.size()) {
                    std::uint64_t id = 0;
                    for (int b = 0; b < 8; ++b) {
                        id |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[doc.dataStart + offset + b])) << (b * 8);
                    }
                    if (id != 0) refs.resourceRefs.push_back(id);
                }
            }
            return refs;
        };
        for (const auto& relative : files->second.tocs) {
            std::error_code error;
            const auto baseToc = baseRoot / fs::path(relative);
            if (!fs::is_regular_file(baseToc, error)) continue;
            try {
                const auto own = fb::read_toc(read_file(mod->directory / fs::path(relative)));
                const auto game = fb::read_toc(read_file(baseToc));
                std::map<std::string, const fb::TocBundle*, std::less<>> gameBundles;
                for (const auto& bundle : game.bundles) gameBundles.emplace(lower(bundle.name), &bundle);
                const auto chunk_changed = [&](const fb::TocChunk& modChunk, const fb::TocChunk& baseChunk) {
                    if (modChunk.size != baseChunk.size) return true;
                    if (modChunk.location == baseChunk.location && modChunk.offset == baseChunk.offset) return false;
                    try {
                        const auto modBytes = store.read(modChunk.location.patch ? mod->directory : baseRoot,
                                                         modChunk.location, modChunk.offset, modChunk.size);
                        const auto gameBytes = store.read(baseRoot, baseChunk.location, baseChunk.offset, baseChunk.size);
                        return modBytes != gameBytes;
                    } catch (const std::exception&) {
                        return true;
                    }
                };
                std::map<fb::Guid, const fb::TocChunk*> gameChunks;
                for (const auto& chunk : game.chunks) gameChunks.emplace(chunk.guid, &chunk);
                for (const auto& chunk : own.chunks) {
                    if (chunk.removed || !chunk.location.patch) continue;
                    const auto shipped = gameChunks.find(chunk.guid);
                    if (shipped == gameChunks.end() || chunk_changed(chunk, *shipped->second)) {
                        if (std::find_if(newChunks.begin(), newChunks.end(), [&](const auto& c) { return c.guid == chunk.guid; }) == newChunks.end())
                            newChunks.push_back(chunk);
                    }
                }
                for (const auto& bundle : own.bundles) {
                    const auto shipped = gameBundles.find(lower(bundle.name));
                    if (shipped == gameBundles.end()) continue;
                    const auto modListing = list_bundle(store, mod->directory, baseRoot, bundle, gameRoot);
                    const auto gameListing = list_bundle(store, baseRoot, baseRoot, *shipped->second, gameRoot);
                    if (!modListing || !gameListing) continue;
                    std::map<std::string, fb::Sha1, std::less<>> original;
                    for (const auto& asset : gameListing->manifest.ebx) original.emplace(lower(asset.name), asset.sha1);
                    for (std::size_t index = 0; index < modListing->manifest.ebx.size(); ++index) {
                        const auto& asset = modListing->manifest.ebx[index];
                        const auto name = lower(asset.name);
                        const auto game_copy = original.find(name);
                        if (game_copy != original.end() && game_copy->second == asset.sha1) continue;
                        const auto at = modListing->first + index;
                        if (at >= modListing->files.size()) continue;
                        const auto& file = modListing->files[at];
                        const auto payload = [&] {
                            return store.read(file.location.patch ? mod->directory : baseRoot,
                                              file.location, file.offset, file.size);
                        };
                        if (game_copy == original.end()) {
                            candidates.push_back({lower(bundle.name), {mod->name, asset, payload(), lower(relative)}});
                            continue;
                        }
                        auto& versions = out.changed[lower(bundle.name)][asset_key(asset)];
                        if (versions.contains(game_copy->second)) continue;
                        const auto& change = versions.emplace(game_copy->second,
                            AssetOverride{mod->name, asset, payload()}).first->second;
                        ++changed;
                        try {
                            const auto refs = parse_ebx(change.encoded);
                            imported.insert(refs.imports.begin(), refs.imports.end());
                            referencedResources.insert(refs.resourceRefs.begin(), refs.resourceRefs.end());
                        } catch (const std::exception&) {}
                    }
                    // Resources: unchanged copies take changes other mods make; added resources follow companions.
                    std::map<std::string, const fb::BundleAsset*, std::less<>> originalResources;
                    for (const auto& asset : gameListing->manifest.resources) originalResources.emplace(lower(asset.name), &asset);
                    for (std::size_t index = 0; index < modListing->manifest.resources.size(); ++index) {
                        const auto& asset = modListing->manifest.resources[index];
                        const auto at = modListing->first + modListing->manifest.ebx.size() + index;
                        if (at >= modListing->files.size()) continue;
                        const auto& file = modListing->files[at];
                        const auto payload = [&] {
                            return store.read(file.location.patch ? mod->directory : baseRoot,
                                              file.location, file.offset, file.size);
                        };
                        const auto gameRes = originalResources.find(lower(asset.name));
                        if (gameRes == originalResources.end()) {
                            companions.push_back({lower(bundle.name), {mod->name, asset, payload(), lower(relative)}});
                            continue;
                        }
                        const auto same = gameRes->second->sha1 == asset.sha1 &&
                                          gameRes->second->originalSize == asset.originalSize &&
                                          gameRes->second->resourceType == asset.resourceType &&
                                          gameRes->second->resourceId == asset.resourceId &&
                                          gameRes->second->resourceMeta == asset.resourceMeta;
                        if (same) continue;
                        auto& versions = out.changed[lower(bundle.name)][asset_key(asset)];
                        if (versions.contains(gameRes->second->sha1)) continue;
                        versions.emplace(gameRes->second->sha1, AssetOverride{mod->name, asset, payload()});
                        ++changed;
                    }
                }
            } catch (const std::exception& failure) {
                report.notes.push_back(mod->name + ": " + relative +
                    ": its changes could not be read for other mods' copies (" + failure.what() + ")");
            }
        }
        for (const auto& chunk : newChunks)
            out.modChunks.push_back({mod->name, chunk});

        // One addition per kind and name in a bundle, the highest-priority mod's. Two
        // mods adding the same asset is no clash; two different assets under one name
        // is, and only one of them can be what a copy gets: say whose, so a song or an
        // item that goes missing on a map can be traced to the mod that took its name.
        std::map<std::string, std::pair<std::size_t, std::string>, std::less<>> shadowed;   // by the mod kept
        const auto keep = [&](const std::string& bundle, AssetAddition addition) {
            auto& list = out.added[bundle];
            const auto name = lower(addition.asset.name);
            const auto holder = std::ranges::find_if(list, [&](const AssetAddition& other) {
                return other.asset.kind == addition.asset.kind && lower(other.asset.name) == name; });
            if (holder != list.end()) {
                if (holder->mod != addition.mod && holder->asset.sha1 != addition.asset.sha1) {
                    auto& [count, example] = shadowed[holder->mod];
                    if (!count++) example = addition.asset.name;
                }
                return false;
            }
            list.push_back(std::move(addition));
            return true;
        };
        // Transitively: an addition a following addition imports follows too (a new song
        // imports its new wave, and only the song is named by the changed playlist).
        struct Candidate {
            fb::Guid file;
            std::vector<fb::Guid> imports;
            std::vector<std::uint64_t> resourceRefs;
            bool taken{};
        };
        std::vector<Candidate> parsed(candidates.size());
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            try {
                const auto refs = parse_ebx(candidates[index].second.encoded);
                parsed[index].file = refs.fileGuid;
                parsed[index].imports = std::move(refs.imports);
                parsed[index].resourceRefs = std::move(refs.resourceRefs);
            } catch (const std::exception&) { parsed[index].taken = true; }   // unreadable: never follows
        }
        for (bool grew = true; grew;) {
            grew = false;
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                auto& candidate = parsed[index];
                if (candidate.taken || !imported.contains(candidate.file)) continue;
                candidate.taken = grew = true;
                imported.insert(candidate.imports.begin(), candidate.imports.end());
                referencedResources.insert(candidate.resourceRefs.begin(), candidate.resourceRefs.end());
                auto& [bundle, addition] = candidates[index];
                keep(bundle, std::move(addition));
            }
        }
        for (auto& [bundle, companion] : companions) {
            bool match = false;
            if (companion.asset.resourceId != 0 && referencedResources.contains(companion.asset.resourceId)) {
                match = true;
            } else {
                const auto found = out.added.find(bundle);
                if (found != out.added.end()) {
                    const auto name = lower(companion.asset.name);
                    match = std::ranges::any_of(found->second, [&](const AssetAddition& addition) {
                        return addition.mod == mod->name && addition.asset.kind == fb::AssetKind::ebx &&
                               lower(addition.asset.name) == name;
                    });
                }
            }
            if (match) keep(bundle, std::move(companion));
        }
        for (const auto& [other, clash] : shadowed)
            report.notes.push_back(mod->name + ": " + std::to_string(clash.first) +
                " added asset(s) share a name with ones " + other + " adds, e.g. " + clash.second +
                "; other mods' copies of the bundle get " + other + "'s");
        // The new TOC chunks this mod's carried EBX name (as raw GUID bytes, the way an
        // EBX stores a ChunkId), so they can follow those assets into other superbundles.
        // Chunk dependencies attach to the selected asset version (changed or added),
        // so only successfully propagated changes bring their required chunks.
        if (!newChunks.empty()) {
            std::set<fb::Guid> taken;
            for (auto& [bundleName, bundleChanged] : out.changed) {
                for (auto& [name, versions] : bundleChanged) {
                    for (auto& [sha1, change] : versions) {
                    if (change.mod != mod->name) continue;
                    try {
                        const auto decodedPayload = fb::decode_cas(change.encoded, {gameRoot});
                        for (const auto& chunk : newChunks) {
                            const auto& id = chunk.guid.bytes;
                            if (std::search(decodedPayload.begin(), decodedPayload.end(), id.begin(), id.end()) != decodedPayload.end()) {
                                if (std::find_if(change.chunks.begin(), change.chunks.end(), [&](const auto& c) { return c.guid == chunk.guid; }) == change.chunks.end()) {
                                    change.chunks.push_back(chunk);
                                    taken.insert(chunk.guid);
                                }
                            }
                        }
                    } catch (const std::exception&) {}
                }
            }
        }
        for (auto& [bundle, list] : out.added) {
                for (auto& addition : list) {
                    if (addition.mod != mod->name || (addition.asset.kind != fb::AssetKind::ebx && addition.asset.kind != fb::AssetKind::resource)) continue;
                    try {
                        const auto decodedPayload = fb::decode_cas(addition.encoded, {gameRoot});
                        for (const auto& chunk : newChunks) {
                            const auto& id = chunk.guid.bytes;
                            if (std::search(decodedPayload.begin(), decodedPayload.end(), id.begin(), id.end()) != decodedPayload.end()) {
                                if (std::find_if(addition.chunks.begin(), addition.chunks.end(), [&](const auto& c) { return c.guid == chunk.guid; }) == addition.chunks.end()) {
                                    addition.chunks.push_back(chunk);
                                    taken.insert(chunk.guid);
                                }
                            }
                        }
                    } catch (const std::exception&) {}
                }
            }
            if (!taken.empty())
                report.notes.push_back(mod->name + ": " + std::to_string(taken.size()) +
                    " added chunk(s) follow its changes into other mods' superbundles");
        }
        if (changed)
            report.notes.push_back(mod->name + ": " + std::to_string(changed) +
                " changed asset(s) also apply to the copies other mods carry");
    }
    return out;
}

} // namespace dingosdk::mods::detail
