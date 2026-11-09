
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
constexpr char kPageMagic[4] = {'C', 'A', 'T', '1'};
constexpr std::uint32_t kFormat = 1;

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

        if (table.first_page_id < 2) {
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

page_id_t ReadNext(const Page& page) {
    std::uint32_t value = 0;

    for (int i = 0; i < 4; ++i) {
        value |=
            std::uint32_t(
                static_cast<unsigned char>(page.data[4 + i])
            ) << (8 * i);
    }

    return static_cast<page_id_t>(value);
}

std::uint32_t ReadUsed(const Page& page) {
    std::uint32_t value = 0;

    for (int i = 0; i < 4; ++i) {
        value |=
            std::uint32_t(
                static_cast<unsigned char>(page.data[8 + i])
            ) << (8 * i);
    }

    return value;
}

void Put32(char* destination, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        destination[i] =
            static_cast<char>((value >> (8 * i)) & 255);
    }
}

bool ReadChain(
    page_id_t root,
    BufferPoolManager& bpm,
    std::vector<std::uint8_t>* output,
    std::vector<page_id_t>* page_ids
) {
    std::set<page_id_t> seen;

    while (root != INVALID_PAGE_ID) {
        if (root < 2 || !seen.insert(root).second) {
            return false;
        }

        Page* page = bpm.FetchPage(root);

        if (page == nullptr) {
            return false;
        }

        bool valid =
            std::memcmp(page->data, kPageMagic, 4) == 0;

        auto used = ReadUsed(*page);
        auto next = ReadNext(*page);

        if (used > PAGE_SIZE - kPageHeader) {
            valid = false;
        }

        try {
            if (valid) {
                output->insert(
                    output->end(),
                    reinterpret_cast<std::uint8_t*>(
                        page->data + kPageHeader
                    ),
                    reinterpret_cast<std::uint8_t*>(
                        page->data + kPageHeader + used
                    )
                );
            }
        } catch (...) {
            bpm.UnpinPage(root, false);
            throw;
        }

        bpm.UnpinPage(root, false);

        if (!valid) {
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

        Decode(bytes, catalog);

        return true;

    } catch (const std::exception& exception) {

        if (error != nullptr) {
            *error = exception.what();
        }

        return false;
    }
}

}  // namespace ocera
