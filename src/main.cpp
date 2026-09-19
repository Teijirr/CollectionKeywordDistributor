#include <SimpleIni.h>
#include <unordered_set>

namespace
{
	struct IniEntry
	{
		std::string file;
		std::string section;
		std::string key;
		std::string value;
	};

	struct DistributionRule
	{
		std::string file;
		std::string section;
		std::vector<std::string> plugins;
		std::vector<std::string> keywords;
	};

	std::vector<IniEntry> g_entries;
	std::vector<DistributionRule> g_rules;

	std::string Trim(std::string_view a_str)
	{
		constexpr auto whitespace = " \t\r\n";
		const auto first = a_str.find_first_not_of(whitespace);
		if (first == std::string_view::npos) {
			return {};
		}
		const auto last = a_str.find_last_not_of(whitespace);
		return std::string(a_str.substr(first, last - first + 1));
	}

	std::vector<std::string> SplitCSV(std::string_view a_str)
	{
		std::vector<std::string> result;
		std::size_t start = 0;
		while (start <= a_str.size()) {
			auto end = a_str.find(',', start);
			if (end == std::string_view::npos) {
				end = a_str.size();
			}
			auto token = Trim(a_str.substr(start, end - start));
			if (!token.empty()) {
				result.push_back(std::move(token));
			}
			start = end + 1;
		}
		return result;
	}

	void ReadIniFile(const std::filesystem::path& a_path)
	{
		CSimpleIniA ini;
		ini.SetUnicode();

		if (ini.LoadFile(a_path.string().c_str()) < 0) {
			SKSE::log::warn("Failed to parse INI: {}", a_path.string());
			return;
		}

		const auto fileName = a_path.filename().string();

		CSimpleIniA::TNamesDepend sections;
		ini.GetAllSections(sections);
		sections.sort(CSimpleIniA::Entry::LoadOrder());

		for (const auto& section : sections) {
			CSimpleIniA::TNamesDepend keys;
			ini.GetAllKeys(section.pItem, keys);
			keys.sort(CSimpleIniA::Entry::LoadOrder());

			for (const auto& key : keys) {
				const char* value = ini.GetValue(section.pItem, key.pItem, "");
				g_entries.push_back({ fileName, section.pItem, key.pItem, Trim(value) });
			}
		}
	}

	void LoadSettings()
	{
		namespace fs = std::filesystem;

		g_entries.clear();

		const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
		const fs::path dir = fs::path("Data/SKSE/Plugins") / std::string(plugin->GetName());

		std::error_code ec;
		if (!fs::is_directory(dir, ec)) {
			SKSE::log::info("Config folder not found: {}", dir.string());
			return;
		}

		std::vector<fs::path> files;
		for (const auto& entry : fs::directory_iterator(dir, ec)) {
			if (!entry.is_regular_file()) {
				continue;
			}
			auto ext = entry.path().extension().string();
			std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (ext == ".ini") {
				files.push_back(entry.path());
			}
		}

		std::ranges::sort(files);

		for (const auto& file : files) {
			ReadIniFile(file);
		}

		SKSE::log::info("Loaded {} INI file(s), {} entries", files.size(), g_entries.size());
	}

	std::string ToLower(std::string a_str)
	{
		std::ranges::transform(a_str, a_str.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return a_str;
	}

	// True if the record exists in a file
	bool IsRecordInFile(const RE::TESForm* a_form, const RE::TESFile* a_file)
	{
		const auto* files = a_form->sourceFiles.array;
		if (!files) {
			return false;
		}
		for (const auto* file : *files) {
			if (file == a_file) {
				return true;
			}
		}
		return false;
	}

	void BuildRules()
	{
		g_rules.clear();

		for (const auto& e : g_entries) {
			const auto key = ToLower(e.key);
			const bool isPlugins = key == "spluginnames";
			const bool isKeywords = key == "skeywordstoadd";
			if (!isPlugins && !isKeywords) {
				continue;
			}

			DistributionRule* rule = nullptr;
			for (auto& r : g_rules) {
				if (r.file == e.file && r.section == e.section) {
					rule = &r;
					break;
				}
			}
			if (!rule) {
				rule = &g_rules.emplace_back(DistributionRule{ e.file, e.section, {}, {} });
			}

			auto& target = isPlugins ? rule->plugins : rule->keywords;
			for (auto& token : SplitCSV(e.value)) {
				target.push_back(std::move(token));
			}
		}
	}

	RE::BGSKeyword* GetOrCreateKeyword(RE::TESDataHandler* a_dataHandler, const std::string& a_editorID)
	{
		auto& keywordArray = a_dataHandler->GetFormArray<RE::BGSKeyword>();

		for (auto* keyword : keywordArray) {
			if (!keyword) {
				continue;
			}
			const char* id = keyword->formEditorID.c_str();
			if (id && _stricmp(id, a_editorID.c_str()) == 0) {
				return keyword;
			}
		}

		auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSKeyword>();
		auto* keyword = factory ? factory->Create() : nullptr;
		if (!keyword) {
			SKSE::log::error("Failed to create keyword '{}'", a_editorID);
			return nullptr;
		}

		keyword->formEditorID = a_editorID.c_str();

		keywordArray.push_back(keyword);

		SKSE::log::info("  created keyword '{}' ({:08X})", a_editorID, keyword->GetFormID());
		return keyword;
	}

	std::vector<RE::BGSKeyword*> ResolveKeywords(RE::TESDataHandler* a_dataHandler, const DistributionRule& a_rule)
	{
		std::vector<RE::BGSKeyword*> result;

		for (const auto& editorID : a_rule.keywords) {
			if (auto* keyword = GetOrCreateKeyword(a_dataHandler, editorID)) {
				result.push_back(keyword);
			}
		}
		return result;
	}

	void ApplyRule(RE::TESDataHandler* a_dataHandler, const DistributionRule& a_rule)
	{
		SKSE::log::info("=== {} [{}] ===", a_rule.file, a_rule.section);

		if (a_rule.plugins.empty() || a_rule.keywords.empty()) {
			SKSE::log::warn("  needs both sPluginNames and sKeywordsToAdd, skipping");
			return;
		}

		const auto keywords = ResolveKeywords(a_dataHandler, a_rule);
		if (keywords.empty()) {
			SKSE::log::warn("  no valid keywords, skipping");
			return;
		}

		std::size_t armorsTouched = 0;
		std::size_t keywordsAdded = 0;
		std::size_t skippedLeveled = 0;

		for (const auto& pluginName : a_rule.plugins) {
			const auto* file = a_dataHandler->LookupModByName(pluginName);
			if (!file) {
				SKSE::log::warn("  plugin '{}' is not loaded, skipping", pluginName);
				continue;
			}

			for (const auto* outfit : a_dataHandler->GetFormArray<RE::BGSOutfit>()) {
				if (!outfit || !IsRecordInFile(outfit, file)) {
					continue;
				}

				for (auto* item : outfit->outfitItems) {
					if (!item) {
						continue;
					}

					auto* armor = item->As<RE::TESObjectARMO>();
					if (!armor) {
						if (item->GetFormType() == RE::FormType::LeveledItem) {
							++skippedLeveled;
						}
						continue;
					}

					bool touched = false;
					for (auto* keyword : keywords) {
						if (armor->HasKeyword(keyword)) {
							continue;
						}
						if (armor->AddKeyword(keyword)) {
							++keywordsAdded;
							touched = true;
							SKSE::log::debug("  + {} -> {:08X} '{}'", keyword->GetFormEditorID(), armor->GetFormID(), armor->GetName());
						}
					}
					if (touched) {
						++armorsTouched;
					}
				}
			}
		}

		SKSE::log::info("  -> added {} keyword(s) to {} armor(s), {} leveled item(s) not expanded", keywordsAdded, armorsTouched, skippedLeveled);
	}

	void OnDataLoaded()
	{
		auto* dataHandler = RE::TESDataHandler::GetSingleton();
		if (!dataHandler) {
			SKSE::log::error("TESDataHandler not available");
			return;
		}

		BuildRules();
		SKSE::log::info("{} rule(s) built from INIs", g_rules.size());

		for (const auto& rule : g_rules) {
			ApplyRule(dataHandler, rule);
		}
	}

	void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			OnDataLoaded();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	LoadSettings();

	auto messaging = SKSE::GetMessagingInterface();
	if (!messaging->RegisterListener("SKSE", MessageHandler)) {
		return false;
	}

	return true;
}