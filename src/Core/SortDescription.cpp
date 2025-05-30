#include <Core/Block.h>
#include <Core/SortDescription.h>
#include <IO/Operators.h>
#include <Columns/IColumn.h>
#include <Common/JSONBuilder.h>
#include <Common/SipHash.h>
#include <Common/typeid_cast.h>
#include <Common/logger_useful.h>

#include "config.h"

#if USE_EMBEDDED_COMPILER
#include <DataTypes/Native.h>
#include <Interpreters/JIT/compileFunction.h>
#include <Interpreters/JIT/CompiledExpressionCache.h>
#endif

namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
}

void dumpSortDescription(const SortDescription & description, WriteBuffer & out)
{
    bool first = true;

    for (const auto & desc : description)
    {
        if (!first)
            out << ", ";
        first = false;

        out << desc.column_name;

        if (desc.direction > 0)
            out << " ASC";
        else
            out << " DESC";

        if (desc.with_fill)
            out << " WITH FILL";
    }
}

void SortColumnDescription::explain(JSONBuilder::JSONMap & map) const
{
    map.add("Column", column_name);
    map.add("Ascending", direction > 0);
    map.add("With Fill", with_fill);
}

bool SortDescription::hasPrefix(const SortDescription & prefix) const
{
    if (prefix.empty())
        return true;

    if (prefix.size() > size())
        return false;

    for (size_t i = 0; i < prefix.size(); ++i)
    {
        if ((*this)[i] != prefix[i])
            return false;
    }
    return true;
}

SortDescription commonPrefix(const SortDescription & lhs, const SortDescription & rhs)
{
    size_t i = 0;
    for (; i < std::min(lhs.size(), rhs.size()); ++i)
    {
        if (lhs[i] != rhs[i])
            break;
    }

    auto res = lhs;
    res.erase(res.begin() + i, res.end());
    return res;
}

#if USE_EMBEDDED_COMPILER

static CHJIT & getJITInstance()
{
    static CHJIT jit;
    return jit;
}

class CompiledSortDescriptionFunctionHolder final : public CompiledExpressionCacheEntry
{
public:
    explicit CompiledSortDescriptionFunctionHolder(CompiledSortDescriptionFunction compiled_function_)
        : CompiledExpressionCacheEntry(compiled_function_.compiled_module.size)
        , compiled_sort_description_function(compiled_function_)
    {}

    ~CompiledSortDescriptionFunctionHolder() override
    {
        getJITInstance().deleteCompiledModule(compiled_sort_description_function.compiled_module);
    }

    CompiledSortDescriptionFunction compiled_sort_description_function;
};

static std::string getSortDescriptionDump(const SortDescription & description, const DataTypes & header_types)
{
    WriteBufferFromOwnString buffer;

    for (size_t i = 0; i < description.size(); ++i)
    {
        if (i != 0)
            buffer << ", ";

        buffer << "(type: " << header_types[i]->getName()
            << ", direction: " << description[i].direction
            << ", nulls_direction: " << description[i].nulls_direction
            << ")";
    }

    return buffer.str();
}

static LoggerPtr getLogger()
{
    return ::getLogger("SortDescription");
}

void compileSortDescriptionIfNeeded(SortDescription & description, const DataTypes & sort_description_types, bool increase_compile_attempts)
{
    static std::unordered_map<UInt128, UInt64, UInt128Hash> counter;
    static std::mutex mutex;

    if (!description.compile_sort_description || sort_description_types.empty())
        return;

    for (const auto & type : sort_description_types)
    {
        if (!type->createColumn()->isComparatorCompilable() || !canBeNativeType(*type))
            return;
    }

    auto description_dump = getSortDescriptionDump(description, sort_description_types);

    SipHash sort_description_dump_hash;
    sort_description_dump_hash.update(description_dump);

    const auto sort_description_hash_key = sort_description_dump_hash.get128();

    {
        std::lock_guard lock(mutex);
        UInt64 & current_counter = counter[sort_description_hash_key];
        if (current_counter < description.min_count_to_compile_sort_description)
        {
            current_counter += static_cast<UInt64>(increase_compile_attempts);
            return;
        }
    }

    std::shared_ptr<CompiledSortDescriptionFunctionHolder> compiled_sort_description_holder;

    if (auto * compilation_cache = CompiledExpressionCacheFactory::instance().tryGetCache())
    {
        auto [compiled_function_cache_entry, _] = compilation_cache->getOrSet(sort_description_hash_key, [&] ()
        {
            LOG_TRACE(getLogger(), "Compile sort description {}", description_dump);

            auto compiled_sort_description = compileSortDescription(getJITInstance(), description, sort_description_types, description_dump);
            return std::make_shared<CompiledSortDescriptionFunctionHolder>(std::move(compiled_sort_description));
        });

        compiled_sort_description_holder = std::static_pointer_cast<CompiledSortDescriptionFunctionHolder>(compiled_function_cache_entry);
    }
    else
    {
        LOG_TRACE(getLogger(), "Compile sort description {}", description_dump);
        auto compiled_sort_description = compileSortDescription(getJITInstance(), description, sort_description_types, description_dump);
        compiled_sort_description_holder = std::make_shared<CompiledSortDescriptionFunctionHolder>(std::move(compiled_sort_description));
    }

    auto comparator_function = compiled_sort_description_holder->compiled_sort_description_function.comparator_function;
    description.compiled_sort_description = reinterpret_cast<void *>(comparator_function);
    description.compiled_sort_description_holder = std::move(compiled_sort_description_holder);
}

#else

void compileSortDescriptionIfNeeded(SortDescription & description, const DataTypes & sort_description_types, bool increase_compile_attempts)
{
    (void)(description);
    (void)(sort_description_types);
    (void)(increase_compile_attempts);
}

#endif

std::string dumpSortDescription(const SortDescription & description)
{
    WriteBufferFromOwnString wb;
    dumpSortDescription(description, wb);
    return wb.str();
}

JSONBuilder::ItemPtr explainSortDescription(const SortDescription & description)
{
    auto json_array = std::make_unique<JSONBuilder::JSONArray>();
    for (const auto & descr : description)
    {
        auto json_map = std::make_unique<JSONBuilder::JSONMap>();
        descr.explain(*json_map);
        json_array->add(std::move(json_map));
    }

    return json_array;
}

void serializeSortDescription(const SortDescription & sort_description, WriteBuffer & out)
{
    writeVarUInt(sort_description.size(), out);
    for (const auto & desc : sort_description)
    {
        writeStringBinary(desc.column_name, out);

        UInt8 flags = 0;
        if (desc.direction > 0)
            flags |= 1;
        if (desc.nulls_direction > 0)
            flags |= 2;
        if (desc.collator)
            flags |= 4;
        if (desc.with_fill)
            flags |= 8;

        writeIntBinary(flags, out);

        if (desc.collator)
            writeStringBinary(desc.collator->getLocale(), out);

        if (desc.with_fill)
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "WITH FILL is not supported in serialized sort description");
    }
}

void deserializeSortDescription(SortDescription & sort_description, ReadBuffer & in)
{
    size_t size = 0;
    readVarUInt(size, in);
    sort_description.resize(size);
    for (auto & desc : sort_description)
    {
        readStringBinary(desc.column_name, in);
        UInt8 flags = 0;
        readIntBinary(flags, in);

        desc.direction = (flags & 1) ? 1 : -1;
        desc.nulls_direction = (flags & 2) ? 1 : -1;

        if (flags & 4)
        {
            String collator_locale;
            readStringBinary(collator_locale, in);
            if (!collator_locale.empty())
                desc.collator = std::make_shared<Collator>(collator_locale);
        }

        if (flags & 8)
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "WITH FILL is not supported in deserialized sort description");
    }
}

}
