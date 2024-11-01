#pragma once

#include <string>
#include <fstream>
#include <vector>
#include <optional>
#include <filesystem>
#include <string_view>
#include <unordered_map>

struct offsets_t
{
	long block_size;
	char block_type[2];
	short count;
	long first;
	long hash;
};

struct key_block_t
{
	long block_size;
	char block_type[2];
	char dummya[18];
	int subkey_count;
	char dummyb[4];
	int subkeys;
	char dummyc[4];
	int value_count;
	int offsets;
	char dummyd[28];
	short len;
	short du;
	char name[255];
};

struct value_block_t
{
	long block_size;
	char block_type[2];
	short name_len;
	long size;
	long offset;
	long value_type;
	short flags;
	short dummy;
	char name[255];
};

namespace detail
{
	std::vector<char> read_file(const std::filesystem::path& file_path)
	{
		std::ifstream file(file_path, std::ios::binary);
		if (!file.is_open())
		{
			return {};
		}

		return {std::istreambuf_iterator(file), std::istreambuf_iterator<char>()};
	}

	struct string_hash
	{
		using is_transparent = void;

		size_t operator()(const std::string_view str) const
		{
			constexpr std::hash<std::string_view> hasher{};
			return hasher(str);
		}
	};

	template <typename T>
	using unordered_string_map = std::unordered_map<std::string, T, string_hash, std::equal_to<>>;
}

class hive_key_t
{
	key_block_t* key_block;
	uintptr_t main_root;

public:
	explicit hive_key_t(): key_block(nullptr), main_root(0)
	{
	}

	explicit hive_key_t(key_block_t* a, const uintptr_t b): key_block(a), main_root(b)
	{
	}

	[[nodiscard]] std::vector<std::string_view> subkeys_list() const
	{
		const auto item = reinterpret_cast<offsets_t*>(this->main_root + key_block->subkeys);
		if (item->block_type[1] != 'f' && item->block_type[1] != 'h')
			return {};

		std::vector<std::string_view> out;
		for (auto i = 0; i < key_block->subkey_count; i++)
		{
			const auto subkey = reinterpret_cast<key_block_t*>((&item->first)[i * 2] + this->main_root);
			if (!subkey)
				continue;

			out.emplace_back(subkey->name, subkey->len);
		}

		return out;
	}

	[[nodiscard]] std::vector<std::string_view> keys_list() const
	{
		if (!key_block->value_count)
			return {};

		std::vector<std::string_view> out;
		for (auto i = 0; i < key_block->value_count; i++)
		{
			const auto value = reinterpret_cast<value_block_t*>(reinterpret_cast<int*>(key_block->offsets + this->
				main_root + 4)[i] + this->main_root);
			if (!value)
				continue;

			out.emplace_back(value->name, value->name_len);
		}

		return out;
	}

	template <class T>
	std::optional<T> get_key_value(const std::string_view& name)
	{
		for (auto i = 0; i < key_block->value_count; i++)
		{
			const auto value = reinterpret_cast<value_block_t*>(reinterpret_cast<int*>(key_block->offsets + this->
				main_root + 4)[i] + this->main_root);
			if (!value || std::string_view(value->name, value->name_len) != name)
				continue;

			auto data = reinterpret_cast<char*>(this->main_root + value->offset + 4);
			if (value->size & 1 << 31)
				data = reinterpret_cast<char*>(&value->offset);

			if constexpr (std::is_same_v<T, std::string_view>)
			{
				if (value->value_type != REG_SZ && value->value_type != REG_EXPAND_SZ)
					return std::nullopt;

				return std::string_view(data, value->size & 0xffff);
			}
			else if constexpr (std::is_same_v<T, std::vector<std::string_view>>)
			{
				if (value->value_type != REG_MULTI_SZ)
					return std::nullopt;

				std::string_view text;
				std::vector<std::string_view> out;
				for (auto j = 0; j < (value->size & 0xffff); j++)
				{
					if (data[j] == '\0' && data[j + 1] == '\0' && data[j + 2] == '\0')
					{
						if (!text.empty())
							out.emplace_back(text);
						text = {};
					}
					else
					{
						text = std::string_view(data + j - text.size(), text.size() + 1);
					}
				}

				return out;
			}
			else if constexpr (std::is_same_v<T, int>)
			{
				if (value->value_type != REG_DWORD)
					return std::nullopt;

				return *reinterpret_cast<T*>(data);
			}
			else if constexpr (std::is_same_v<T, std::basic_string_view<uint8_t>>)
			{
				if (value->value_type != REG_BINARY)
					return std::nullopt;

				return {data, value->size & 0xffff};
			}
		}

		return std::nullopt;
	}
};

class hive_parser
{
	struct hive_subpaths_t
	{
		std::string path;
		hive_key_t data;
	};

	struct hive_cache_t
	{
		hive_key_t data;
		std::vector<hive_subpaths_t> subpaths;
	};

	key_block_t* main_key_block_data;
	uintptr_t main_root;
	std::vector<char> file_data;
	detail::unordered_string_map<hive_cache_t> subkey_cache;

	void reclusive_search(const key_block_t* key_block_data, const std::string& current_path,
	                      const bool is_reclusive = false)
	{
		if (!key_block_data)
			return;

		const auto item = reinterpret_cast<offsets_t*>(main_root + key_block_data->subkeys);
		if (item->block_type[1] != 'f' && item->block_type[1] != 'h')
			return;

		for (auto i = 0; i < item->count; i++)
		{
			const auto subkey = reinterpret_cast<key_block_t*>((&item->first)[i * 2] + main_root);
			if (!subkey)
				continue;

			std::string_view subkey_name(subkey->name, subkey->len);
			std::string full_path = current_path.empty()
				                        ? std::string(subkey_name)
				                        : std::string(current_path).append("/").append(subkey_name);

			if (!is_reclusive)
				subkey_cache.try_emplace(full_path, hive_cache_t{
					                         hive_key_t{subkey, main_root}, std::vector<hive_subpaths_t>{}
				                         });

			const auto extract_main_key = [ ](const std::string_view str) -> std::string_view
			{
				const size_t slash_pos = str.find('/');
				if (slash_pos == std::string::npos)
					return str;

				return str.substr(0, slash_pos);
			};

			if (subkey->subkey_count > 0)
			{
				reclusive_search(subkey, full_path, true);
				const auto entry = subkey_cache.find(extract_main_key(full_path));
				if (entry == subkey_cache.end())
				{
					throw std::out_of_range("Invalid key");
				}

				entry->second.subpaths.emplace_back(hive_subpaths_t{
					full_path, hive_key_t{subkey, main_root}
				});
			}
			else
			{
				const auto entry = subkey_cache.find(extract_main_key(full_path));
				if (entry == subkey_cache.end())
				{
					throw std::out_of_range("Invalid key");
				}

				entry->second.subpaths.emplace_back(full_path, hive_key_t{subkey, main_root});
			}
		}
	}

public:
	explicit hive_parser(const std::filesystem::path& file_path)
		: hive_parser(detail::read_file(file_path))
	{
	}

	explicit hive_parser(std::vector<char> input_data)
		: file_data(std::move(input_data))
	{
		if (file_data.size() < 0x1020)
			return;

		if (file_data.at(0) != 'r' && file_data.at(1) != 'e' && file_data.at(2) != 'g' && file_data.at(3) != 'f')
			return;

		main_key_block_data = reinterpret_cast<key_block_t*>(reinterpret_cast<uintptr_t>(file_data.data() + 0x1020));
		main_root = reinterpret_cast<uintptr_t>(main_key_block_data) - 0x20;

		reclusive_search(main_key_block_data, "");
	}

	[[nodiscard]] bool success() const
	{
		return !subkey_cache.empty();
	}

	[[nodiscard]] std::optional<hive_key_t> get_subkey(const std::string_view key_name,
	                                                   const std::string_view path) const
	{
		if (!subkey_cache.contains(key_name))
			return std::nullopt;

		const auto hive_block = subkey_cache.find(key_name);
		if (hive_block == subkey_cache.end())
		{
			throw std::out_of_range("Invalid key");
		}

		for (const auto& hive : hive_block->second.subpaths)
		{
			if (hive.path == path)
				return hive.data;
		}

		return std::nullopt;
	}
};
