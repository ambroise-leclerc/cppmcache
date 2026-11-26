// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file Reapi.cppm
 * @brief Subset of the Remote Execution API v2 messages, canonically encoded.
 *
 * The field numbers are those of remote_execution.proto (build.bazel.remote.execution.v2).
 */
export module cppmcache.reapi;

import std;
import cppmcache.hash;
import cppmcache.proto;

export namespace cppmcache {

using NameValue = std::pair<std::string, std::string>;

std::string encodeDigest(const Digest& digest) {
    ProtoWriter writer;
    writer.bytes(1, digest.hash);
    writer.varint(2, static_cast<std::uint64_t>(digest.size));
    return writer.take();
}

std::optional<Digest> decodeDigest(std::string_view encoded) {
    Digest      digest;
    ProtoReader reader{encoded};
    while (const auto field = reader.next()) {
        if (field->number == 1 && field->wire == 2) {
            digest.hash = field->data;
        } else if (field->number == 2 && field->wire == 0) {
            digest.size = static_cast<std::int64_t>(field->value);
        }
    }
    if (!reader.ok()) {
        return std::nullopt;
    }
    return digest;
}

struct Command {
    std::vector<std::string> arguments;
    std::vector<NameValue>   environment;  // sorted by name when encoding
    std::vector<std::string> outputPaths;  // sorted when encoding

    std::string encode() const {
        ProtoWriter writer;
        for (const auto& argument : arguments) {
            writer.message(1, argument);
        }
        auto sortedEnvironment = environment;
        std::ranges::sort(sortedEnvironment);
        for (const auto& [name, value] : sortedEnvironment) {
            ProtoWriter variable;
            variable.bytes(1, name);
            variable.bytes(2, value);
            writer.message(2, variable.take());
        }
        auto sortedOutputs = outputPaths;
        std::ranges::sort(sortedOutputs);
        for (const auto& path : sortedOutputs) {
            writer.message(7, path);
        }
        return writer.take();
    }
};

struct Action {
    Digest                 command;
    Digest                 inputRoot;
    std::vector<NameValue> platform;  // sorted by name when encoding

    std::string encode() const {
        ProtoWriter writer;
        writer.message(1, encodeDigest(command));
        writer.message(2, encodeDigest(inputRoot));
        if (!platform.empty()) {
            auto sorted = platform;
            std::ranges::sort(sorted);
            ProtoWriter properties;
            for (const auto& [name, value] : sorted) {
                ProtoWriter property;
                property.bytes(1, name);
                property.bytes(2, value);
                properties.message(1, property.take());
            }
            writer.message(10, properties.take());
        }
        return writer.take();
    }
};

struct OutputFile {
    std::string path;
    Digest      digest;
};

struct ActionResult {
    std::vector<OutputFile> outputFiles;
    int                     exitCode = 0;
    std::string             stdoutRaw;
    std::string             stderrRaw;

    std::string encode() const {
        ProtoWriter writer;
        for (const auto& file : outputFiles) {
            ProtoWriter output;
            output.bytes(1, file.path);
            output.message(2, encodeDigest(file.digest));
            writer.message(2, output.take());
        }
        writer.varint(4, static_cast<std::uint64_t>(static_cast<std::int64_t>(exitCode)));
        writer.bytes(5, stdoutRaw);
        writer.bytes(7, stderrRaw);
        return writer.take();
    }

    static std::optional<ActionResult> decode(std::string_view encoded) {
        ActionResult result;
        ProtoReader  reader{encoded};
        while (const auto field = reader.next()) {
            if (field->number == 2 && field->wire == 2) {
                OutputFile  file;
                ProtoReader output{field->data};
                while (const auto part = output.next()) {
                    if (part->number == 1 && part->wire == 2) {
                        file.path = part->data;
                    } else if (part->number == 2 && part->wire == 2) {
                        const auto digest = decodeDigest(part->data);
                        if (!digest) {
                            return std::nullopt;
                        }
                        file.digest = *digest;
                    }
                }
                if (!output.ok()) {
                    return std::nullopt;
                }
                result.outputFiles.push_back(std::move(file));
            } else if (field->number == 4 && field->wire == 0) {
                result.exitCode = static_cast<int>(static_cast<std::int64_t>(field->value));
            } else if (field->number == 5 && field->wire == 2) {
                result.stdoutRaw = field->data;
            } else if (field->number == 7 && field->wire == 2) {
                result.stderrRaw = field->data;
            }
        }
        if (!reader.ok()) {
            return std::nullopt;
        }
        return result;
    }
};

// Input tree of an Action: a Merkle tree of Directory messages.
class InputTree {
public:
    // `path` is split on '/' and '\\'; empty and "." components are ignored.
    void add(std::string_view path, const Digest& digest) {
        Node*                    node = &root_;
        std::string              component;
        std::vector<std::string> components;
        for (const char c : path) {
            if (c == '/' || c == '\\') {
                if (!component.empty() && component != ".") {
                    components.push_back(component);
                }
                component.clear();
            } else {
                component.push_back(c);
            }
        }
        if (component.empty() || component == ".") {
            return;
        }
        for (const auto& name : components) {
            auto& child = node->directories[name];
            if (!child) {
                child = std::make_unique<Node>();
            }
            node = child.get();
        }
        node->files[component] = digest;
    }

    // Digest of the root Directory. If `blobs` is provided, it receives every encoded Directory.
    Digest rootDigest(std::vector<std::string>* blobs = nullptr) const {
        return digestNode(root_, blobs);
    }

private:
    struct Node {
        std::map<std::string, Digest>                files;
        std::map<std::string, std::unique_ptr<Node>> directories;
    };

    static Digest digestNode(const Node& node, std::vector<std::string>* blobs) {
        ProtoWriter writer;
        for (const auto& [name, digest] : node.files) {
            ProtoWriter file;
            file.bytes(1, name);
            file.message(2, encodeDigest(digest));
            writer.message(1, file.take());
        }
        for (const auto& [name, child] : node.directories) {
            ProtoWriter directory;
            directory.bytes(1, name);
            directory.message(2, encodeDigest(digestNode(*child, blobs)));
            writer.message(2, directory.take());
        }
        std::string  encoded = writer.take();
        const Digest digest  = digestOf(encoded);
        if (blobs != nullptr) {
            blobs->push_back(std::move(encoded));
        }
        return digest;
    }

    Node root_;
};

}  // namespace cppmcache
