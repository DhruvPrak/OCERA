
#include "catalog/catalog_storage.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ocera {

struct CatalogStorageAccess {
    static const auto& Tables(const Catalog& catalog) {
        return catalog.tables_;
    }

    static const auto& Indexes(const Catalog& catalog) {
        return catalog.indexes_;
    }

    static void Set(
        Catalog& catalog,
        std::map<std::string, TableSchema> tables,
        std::map<std::string, IndexSchema> indexes
    ) {
        catalog.tables_ = std::move(tables);
        catalog.indexes_ = std::move(indexes);
    }
};

namespace {

constexpr std::size_t kPageHeader = 12;
constexpr std::size_t kLegacyPageHeader = 8;
constexpr char kPageMagic[4] = {'C', 'A', 'T', '1'};
constexpr char kLegacyImageMagic[4] = {'C', 'T', 'A', '1'};
constexpr std::uint32_t kFormat = 1;
constexpr std::uint32_t kLegacyImageMagicValue = 0x31415443U;

struct Writer {
    std::vector<std::uint8_t> data;

    template <class T>
    void Number(T value) {
        using U = typename std::make_unsigned<T>::type;

        U v = static_cast<U>(value);

        for (std::size_t i = 0; i < sizeof(T); ++i) {
            data.push_back(
                static_cast<std::uint8_t>((v >> (i * 8)) & 0xff)
            );
        }
    }

    void Count(std::size_t count) {
        if (count > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("catalog list is too large");
        }

        Number<std::uint32_t>(static_cast<std::uint32_t>(count));
    }

    void String(const std::string& value) {
        Count(value.size());

        data.insert(data.end(), value.begin(), value.end());
    }
};

struct Reader {
    const std::vector<std::uint8_t>& data;
    std::size_t pos = 0;

    template <class T>
    T Number() {
        if (pos > data.size() ||
            data.size() - pos < sizeof(T)) {
            throw std::runtime_error("catalog data is truncated");
        }

        using U = typename std::make_unsigned<T>::type;

        U value = 0;

        for (std::size_t i = 0; i < sizeof(T); ++i) {
            value |= static_cast<U>(data[pos++]) << (i * 8);
        }

        return static_cast<T>(value);
    }

    std::string String() {
        const auto size = Number<std::uint32_t>();

        if (pos > data.size() ||
            size > data.size() - pos) {
            throw std::runtime_error("catalog string is truncated");
        }

        std::string result(
            data.begin() + pos,
            data.begin() + pos + size
        );

        pos += size;

        return result;
    }
};

void WriteStringList(
    Writer& writer,
    const std::vector<std::string>& values
) {
    writer.Count(values.size());

    for (const auto& value : values) {
        writer.String(value);
    }
}

std::vector<std::string> ReadStringList(Reader& reader) {
    auto count = reader.Number<std::uint32_t>();

    if (count > 100000) {
        throw std::runtime_error("catalog list is too large");
    }

    std::vector<std::string> values;
    values.reserve(count);

    while (count--) {
        values.push_back(reader.String());
    }

    return values;
}

std::vector<std::uint8_t> Encode(const Catalog& catalog) {
    Writer writer;

    writer.Number<std::uint32_t>(kFormat);

    const auto& tables = CatalogStorageAccess::Tables(catalog);

    writer.Count(tables.size());

    for (const auto& pair : tables) {
        const auto& table = pair.second;

        writer.String(table.name);
        writer.Number<std::int32_t>(table.first_page_id);

        writer.Count(table.columns.size());

        for (const auto& column : table.columns) {
            writer.String(column.name);

            writer.Number<std::uint8_t>(
                static_cast<std::uint8_t>(column.type)
            );
        }

        writer.Count(table.key_constraints.size());

        for (const auto& key : table.key_constraints) {
            writer.Number<std::uint8_t>(
                static_cast<std::uint8_t>(key.type)
            );

            WriteStringList(writer, key.columns);
        }

        writer.Count(table.foreign_key_constraints.size());

        for (const auto& foreign_key : table.foreign_key_constraints) {
            WriteStringList(writer, foreign_key.columns);

            writer.String(foreign_key.referenced_table);

            WriteStringList(writer, foreign_key.referenced_columns);
        }
    }

    const auto& indexes = CatalogStorageAccess::Indexes(catalog);

    writer.Count(indexes.size());

    for (const auto& pair : indexes) {
        const auto& index = pair.second;

        writer.String(index.name);
        writer.String(index.table_name);
        writer.String(index.column_name);
    }

    return std::move(writer.data);
}

void Decode(
    const std::vector<std::uint8_t>& bytes,
    Catalog& catalog
) {
    Reader reader{bytes};

    if (reader.Number<std::uint32_t>() != kFormat) {
        throw std::runtime_error("unsupported catalog format");
    }

    auto table_count = reader.Number<std::uint32_t>();

    if (table_count > 100000) {
        throw std::runtime_error("too many catalog tables");
    }

    std::map<std::string, TableSchema> tables;

    while (table_count--) {
        TableSchema table;

        table.name = reader.String();
        table.first_page_id = reader.Number<std::int32_t>();

        if (table.first_page_id != INVALID_PAGE_ID &&
            table.first_page_id < 2) {
            throw std::runtime_error(
                "invalid table starting page in catalog"
            );
        }

        auto column_count = reader.Number<std::uint32_t>();

        if (column_count == 0 || column_count > 10000) {
            throw std::runtime_error("invalid catalog column count");
        }

        while (column_count--) {
            ColumnSchema column;

            column.name = reader.String();

            auto type = reader.Number<std::uint8_t>();

            if (type > 1) {
                throw std::runtime_error("invalid column type");
            }

            column.type = static_cast<sql::ColumnType>(type);

            table.columns.push_back(std::move(column));
        }

        auto key_count = reader.Number<std::uint32_t>();

        if (key_count > 10000) {
            throw std::runtime_error("too many key rules");
        }

        while (key_count--) {
            KeySchema key;

            auto type = reader.Number<std::uint8_t>();

            if (type > 1) {
                throw std::runtime_error("invalid key type");
            }

            key.type = static_cast<sql::KeyConstraintType>(type);
            key.columns = ReadStringList(reader);

            table.key_constraints.push_back(std::move(key));
        }

        auto foreign_key_count = reader.Number<std::uint32_t>();

        if (foreign_key_count > 10000) {
            throw std::runtime_error("too many foreign-key rules");
        }

        while (foreign_key_count--) {
            ForeignKeySchema foreign_key;

            foreign_key.columns = ReadStringList(reader);
            foreign_key.referenced_table = reader.String();
            foreign_key.referenced_columns = ReadStringList(reader);

            table.foreign_key_constraints.push_back(
                std::move(foreign_key)
            );
        }

        auto normalized_name = NormalizeIdentifier(table.name);

        if (!tables.emplace(
                normalized_name,
                std::move(table)
            ).second) {
            throw std::runtime_error("duplicate table in catalog");
        }
    }

    auto index_count = reader.Number<std::uint32_t>();

    if (index_count > 100000) {
        throw std::runtime_error("too many catalog indexes");
    }

    std::map<std::string, IndexSchema> indexes;

    while (index_count--) {
        IndexSchema index;

        index.name = reader.String();
        index.table_name = reader.String();
        index.column_name = reader.String();

        auto normalized_name = NormalizeIdentifier(index.name);

        if (!indexes.emplace(
                normalized_name,
                std::move(index)
            ).second) {
            throw std::runtime_error("duplicate index in catalog");
        }
    }

    if (reader.pos != bytes.size()) {
        throw std::runtime_error("catalog has trailing data");
    }

    // Update the Catalog only after the complete data has been read.
    CatalogStorageAccess::Set(
        catalog,
        std::move(tables),
        std::move(indexes)
    );
}

// The original restart-persistence implementation stored a CTA1 image
// (version 1 or 2) inside pages with an 8-byte [next, used] header.
// Version 1 did not persist first_page_id; version 2 did.
void DecodeLegacy(
    const std::vector<std::uint8_t>& bytes,
    Catalog& catalog
) {
    Reader reader{bytes};

    if (reader.Number<std::uint32_t>() != kLegacyImageMagicValue) {
        throw std::runtime_error("invalid legacy catalog image magic");
    }

    const auto version = reader.Number<std::uint32_t>();
    if (version != 1 && version != 2) {
        throw std::runtime_error("unsupported legacy catalog format");
    }

    auto table_count = reader.Number<std::uint32_t>();
    if (table_count > 100000) {
        throw std::runtime_error("too many legacy catalog tables");
    }

    std::map<std::string, TableSchema> tables;
    while (table_count--) {
        TableSchema table;
        table.name = reader.String();

        if (version >= 2) {
            const auto raw_page_id = reader.Number<std::uint32_t>();
            if (raw_page_id == static_cast<std::uint32_t>(INVALID_PAGE_ID)) {
                table.first_page_id = INVALID_PAGE_ID;
            } else if (raw_page_id < 2 ||
                       raw_page_id > static_cast<std::uint32_t>(
                           std::numeric_limits<page_id_t>::max())) {
                throw std::runtime_error(
                    "invalid legacy table starting page in catalog"
                );
            } else {
                table.first_page_id = static_cast<page_id_t>(raw_page_id);
            }
        }

        const auto column_count = reader.Number<std::uint32_t>();
        if (column_count == 0 || column_count > 10000) {
            throw std::runtime_error("invalid legacy catalog column count");
        }

        for (std::uint32_t i = 0; i < column_count; ++i) {
            ColumnSchema column;
            column.name = reader.String();
            const auto type = reader.Number<std::uint32_t>();
            if (type > 1) {
                throw std::runtime_error("invalid legacy catalog column type");
            }
            column.type = static_cast<sql::ColumnType>(type);
            table.columns.push_back(std::move(column));
        }

        auto key_count = reader.Number<std::uint32_t>();
        if (key_count > 10000) {
            throw std::runtime_error("too many legacy catalog key constraints");
        }

        while (key_count--) {
            KeySchema key;
            const auto type = reader.Number<std::uint32_t>();
            if (type > 1) {
                throw std::runtime_error("invalid legacy catalog key type");
            }
            key.type = static_cast<sql::KeyConstraintType>(type);

            auto key_column_count = reader.Number<std::uint32_t>();
            if (key_column_count == 0 || key_column_count > column_count) {
                throw std::runtime_error("invalid legacy key column count");
            }
            while (key_column_count--) {
                key.columns.push_back(reader.String());
            }
            table.key_constraints.push_back(std::move(key));
        }

        auto foreign_key_count = reader.Number<std::uint32_t>();
        if (foreign_key_count > 10000) {
            throw std::runtime_error("too many legacy foreign-key constraints");
        }

        while (foreign_key_count--) {
            ForeignKeySchema foreign_key;
            auto local_count = reader.Number<std::uint32_t>();
            if (local_count == 0 || local_count > column_count) {
                throw std::runtime_error("invalid legacy foreign-key column count");
            }
            while (local_count--) {
                foreign_key.columns.push_back(reader.String());
            }

            foreign_key.referenced_table = reader.String();
            auto referenced_count = reader.Number<std::uint32_t>();
            if (referenced_count != foreign_key.columns.size()) {
                throw std::runtime_error(
                    "invalid legacy referenced foreign-key column count"
                );
            }
            while (referenced_count--) {
                foreign_key.referenced_columns.push_back(reader.String());
            }
            table.foreign_key_constraints.push_back(std::move(foreign_key));
        }

        const auto normalized_name = NormalizeIdentifier(table.name);
        if (normalized_name.empty() ||
            !tables.emplace(normalized_name, std::move(table)).second) {
            throw std::runtime_error("duplicate or empty table in legacy catalog");
        }
    }

    auto index_count = reader.Number<std::uint32_t>();
    if (index_count > 100000) {
        throw std::runtime_error("too many legacy catalog indexes");
    }

    std::map<std::string, IndexSchema> indexes;
    while (index_count--) {
        IndexSchema index;
        index.name = reader.String();
        index.table_name = reader.String();
        index.column_name = reader.String();

        const auto normalized_name = NormalizeIdentifier(index.name);
        if (normalized_name.empty() ||
            !indexes.emplace(normalized_name, std::move(index)).second) {
            throw std::runtime_error("duplicate or empty index in legacy catalog");
        }
    }

    if (reader.pos != bytes.size()) {
        throw std::runtime_error("legacy catalog has trailing data");
    }

    // Match the reference checks performed by the original CTA1 decoder.
    for (const auto& entry : tables) {
        const TableSchema& table = entry.second;
        for (const auto& foreign_key : table.foreign_key_constraints) {
            const auto parent = tables.find(
                NormalizeIdentifier(foreign_key.referenced_table)
            );
            if (parent == tables.end() ||
                foreign_key.columns.size() !=
                    foreign_key.referenced_columns.size()) {
                throw std::runtime_error(
                    "legacy catalog foreign key references a missing table"
                );
            }
            for (std::size_t i = 0; i < foreign_key.columns.size(); ++i) {
                if (table.ColumnIndex(foreign_key.columns[i]) < 0 ||
                    parent->second.ColumnIndex(
                        foreign_key.referenced_columns[i]
                    ) < 0) {
                    throw std::runtime_error(
                        "legacy catalog foreign key references a missing column"
                    );
                }
            }
        }
    }

    for (const auto& entry : indexes) {
        const auto table = tables.find(
            NormalizeIdentifier(entry.second.table_name)
        );
        if (table == tables.end() ||
            table->second.ColumnIndex(entry.second.column_name) < 0) {
            throw std::runtime_error(
                "legacy catalog index references a missing table or column"
            );
        }
    }

    CatalogStorageAccess::Set(
        catalog,
        std::move(tables),
        std::move(indexes)
    );
}

std::uint32_t ReadU32(const char* data, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(
            static_cast<unsigned char>(data[offset + i])
        ) << (8U * i);
    }
    return value;
}

page_id_t ReadPageId(const char* data, std::size_t offset) {
    const std::uint32_t bits = ReadU32(data, offset);
    page_id_t value = INVALID_PAGE_ID;
    static_assert(sizeof(value) == sizeof(bits), "page ID must be 32 bits");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void Put32(char* destination, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        destination[i] = static_cast<char>((value >> (8U * i)) & 0xffU);
    }
}

void DecodeCatalogImage(
    const std::vector<std::uint8_t>& bytes,
    Catalog& catalog
) {
    if (bytes.size() < sizeof(std::uint32_t)) {
        throw std::runtime_error("catalog image is truncated");
    }

    Reader probe{bytes};
    const auto signature = probe.Number<std::uint32_t>();
    if (signature == kFormat) {
        Decode(bytes, catalog);
        return;
    }
    if (signature == kLegacyImageMagicValue) {
        DecodeLegacy(bytes, catalog);
        return;
    }
    throw std::runtime_error("unsupported catalog image signature");
}

enum class CatalogPageFormat {
    CURRENT,
    LEGACY
};

bool ReadChain(
    page_id_t root,
    BufferPoolManager& bpm,
    std::vector<std::uint8_t>* output,
    std::vector<page_id_t>* page_ids
) {
    std::set<page_id_t> seen;
    CatalogPageFormat format = CatalogPageFormat::CURRENT;
    bool format_known = false;

    while (root != INVALID_PAGE_ID) {
        if (root < 2 || !seen.insert(root).second) {
            return false;
        }

        Page* page = bpm.FetchPage(root);
        if (page == nullptr) {
            return false;
        }

        bool valid = true;
        std::size_t header_size = kPageHeader;
        page_id_t next = INVALID_PAGE_ID;
        std::uint32_t used = 0;

        if (!format_known) {
            if (std::memcmp(page->data, kPageMagic, sizeof(kPageMagic)) == 0) {
                format = CatalogPageFormat::CURRENT;
            } else if (std::memcmp(
                           page->data + kLegacyPageHeader,
                           kLegacyImageMagic,
                           sizeof(kLegacyImageMagic)
                       ) == 0) {
                format = CatalogPageFormat::LEGACY;
            } else {
                valid = false;
            }
            format_known = valid;
        }

        if (valid && format == CatalogPageFormat::CURRENT) {
            header_size = kPageHeader;
            valid = std::memcmp(
                page->data, kPageMagic, sizeof(kPageMagic)
            ) == 0;
            next = ReadPageId(page->data, 4);
            used = ReadU32(page->data, 8);
        } else if (valid) {
            header_size = kLegacyPageHeader;
            next = ReadPageId(page->data, 0);
            used = ReadU32(page->data, 4);
        }

        if (valid &&
            (used > PAGE_SIZE - header_size ||
             (next != INVALID_PAGE_ID && next < 2))) {
            valid = false;
        }

        try {
            if (valid) {
                const auto* payload =
                    reinterpret_cast<const std::uint8_t*>(
                        page->data + header_size
                    );
                output->insert(output->end(), payload, payload + used);
            }
        } catch (...) {
            bpm.UnpinPage(root, false);
            throw;
        }

        const bool unpinned = bpm.UnpinPage(root, false);
        if (!unpinned || !valid) {
            return false;
        }

        page_ids->push_back(root);
        root = next;
    }

    return true;
}

}  // namespace

bool CatalogStorage::Save(
    const Catalog& catalog,
    BufferPoolManager& bpm,
    DatabaseHeader& header,
    std::string* error
) {
    std::vector<page_id_t> fresh;
    std::vector<page_id_t> old;
    std::vector<Page*> allocated;

    try {
        auto bytes = Encode(catalog);

        const std::size_t capacity = PAGE_SIZE - kPageHeader;

        const std::size_t count =
            (bytes.size() + capacity - 1) / capacity;

        // Allocate new pages for the saved Catalog.
        for (std::size_t i = 0; i < count; ++i) {
            page_id_t id;

            Page* page = bpm.NewPage(&id);

            if (page == nullptr) {
                throw std::runtime_error(
                    "no page available for saving table list"
                );
            }

            fresh.push_back(id);
            allocated.push_back(page);
        }

        std::size_t offset = 0;

        // Write the Catalog data into the new pages.
        for (std::size_t i = 0; i < fresh.size(); ++i) {
            Page* page = allocated[i];

            std::memset(page->data, 0, PAGE_SIZE);

            std::memcpy(
                page->data,
                kPageMagic,
                4
            );

            Put32(
                page->data + 4,
                static_cast<std::uint32_t>(
                    i + 1 < fresh.size()
                        ? fresh[i + 1]
                        : INVALID_PAGE_ID
                )
            );

            auto size = std::min(
                capacity,
                bytes.size() - offset
            );

            Put32(
                page->data + 8,
                static_cast<std::uint32_t>(size)
            );

            if (size > 0) {
                std::memcpy(
                    page->data + kPageHeader,
                    bytes.data() + offset,
                    size
                );
            }

            offset += size;

            bpm.UnpinPage(fresh[i], true);

            allocated[i] = nullptr;
        }

        // Read the previous Catalog pages before replacing them.
        // This temporary vector automatically releases its memory.
        std::vector<std::uint8_t> old_bytes;

        if (!ReadChain(
                header.GetCatalogRootPageId(),
                bpm,
                &old_bytes,
                &old
            )) {
            throw std::runtime_error(
                "existing table list is damaged"
            );
        }

        // Write the new pages before changing the saved root page.
        bpm.FlushAllPages();

        header.SetCatalogRootPageId(
            fresh.empty()
                ? INVALID_PAGE_ID
                : fresh.front()
        );

        // The old Catalog pages are no longer needed.
        for (auto id : old) {
            try {
                bpm.DeletePage(id);
            } catch (...) {
                // Old pages are harmless if they remain unreachable.
            }
        }

        return true;

    } catch (const std::exception& exception) {

        // Release any pages that were allocated but not unpinned.
        for (std::size_t i = 0; i < fresh.size(); ++i) {
            try {
                if (i < allocated.size() && allocated[i] != nullptr) {
                    bpm.UnpinPage(fresh[i], false);
                }

                bpm.DeletePage(fresh[i]);

            } catch (...) {
                // Preserve the original error.
            }
        }

        if (error != nullptr) {
            *error = exception.what();
        }

        return false;
    }
}

bool CatalogStorage::Load(
    Catalog& catalog,
    BufferPoolManager& bpm,
    const DatabaseHeader& header,
    std::string* error
) {
    if (header.GetCatalogRootPageId() == INVALID_PAGE_ID) {
        return true;
    }

    try {
        std::vector<std::uint8_t> bytes;
        std::vector<page_id_t> page_ids;

        if (!ReadChain(
                header.GetCatalogRootPageId(),
                bpm,
                &bytes,
                &page_ids
            )) {
            throw std::runtime_error(
                "could not read saved table list"
            );
        }

        DecodeCatalogImage(bytes, catalog);

        return true;

    } catch (const std::exception& exception) {

        if (error != nullptr) {
            *error = exception.what();
        }

        return false;
    }
}

}  // namespace ocera
