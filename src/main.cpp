#include <SimpleIni.h>
#include <unordered_set>

namespace
{
	// ------------------------------------------------------------------
	// INI loading
	// ------------------------------------------------------------------

	// key/value pair from one INI file
	struct IniEntry
	{
		std::string file;
		std::string section;
		std::string key;
		std::string value;
	};

	std::vector<IniEntry> g_entries;

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

	std::string ToLower(std::string a_str)
	{
		std::ranges::transform(a_str, a_str.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return a_str;
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
			if (ToLower(entry.path().extension().string()) == ".ini") {
				files.push_back(entry.path());
			}
		}

		std::ranges::sort(files);

		for (const auto& file : files) {
			ReadIniFile(file);
		}

		SKSE::log::info("Loaded {} INI file(s), {} entries", files.size(), g_entries.size());
	}

	// ------------------------------------------------------------------
	// Rules
	// ------------------------------------------------------------------

	// True if the record exists in a_file
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

	struct DistributionRule
	{
		std::string file;
		std::string section;
		std::vector<std::string> plugins;
		std::vector<std::string> keywords;
	};

	std::vector<DistributionRule> g_rules;

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

		// Reuse an existing keyword
		for (auto* keyword : keywordArray) {
			if (!keyword) {
				continue;
			}
			const char* id = keyword->formEditorID.c_str();
			if (id && _stricmp(id, a_editorID.c_str()) == 0) {
				return keyword;
			}
		}

		// Create a keyword
		auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSKeyword>();
		auto* keyword = factory ? factory->Create() : nullptr;
		if (!keyword) {
			SKSE::log::error("Failed to create keyword '{}'", a_editorID);
			return nullptr;
		}

		keyword->formEditorID = a_editorID.c_str();

		keywordArray.push_back(keyword);

		const auto& [map, lock] = RE::TESForm::GetAllFormsByEditorID();
		if (map) {
			RE::BSWriteLockGuard guard{ lock };
			map->emplace(RE::BSFixedString(a_editorID.c_str()), keyword);
		}
		
		if (RE::TESForm::LookupByEditorID(a_editorID) != keyword) {
			SKSE::log::warn("  keyword '{}' is not resolvable by editor ID; other plugins' editor ID filters won't find it", a_editorID);
		}

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

	// ------------------------------------------------------------------
	// Collections: every form that can hold a keyword
	// ------------------------------------------------------------------

	struct KeywordTarget
	{
		RE::TESForm* form;
		RE::BGSKeywordForm* keywordForm;
	};

	// Walks collections recursively and gathers every reachable form that can hold keywords
	// (armor, weapon, ammo, misc, potion, book, scroll, ingredient, soul gem, key, spell, NPC, ...).
	class KeywordTargetCollector
	{
	public:
		void CollectRoot(RE::TESForm* a_form)
		{
			if (auto* npc = a_form->As<RE::TESNPC>()) {
				CollectContainer(npc);
			}
			else {
				Collect(a_form);
			}
		}

		[[nodiscard]] const std::vector<KeywordTarget>& Targets() const { return _targets; }

	private:
		void Collect(RE::TESForm* a_form)
		{
			if (!a_form || !_visited.insert(a_form).second) {
				return;
			}

			if (auto* outfit = a_form->As<RE::BGSOutfit>()) {
				for (auto* item : outfit->outfitItems) {
					Collect(item);
				}
				return;
			}
			if (auto* levItem = a_form->As<RE::TESLevItem>()) {
				CollectLeveled(levItem);
				return;
			}
			if (auto* levChar = a_form->As<RE::TESLevCharacter>()) {
				CollectLeveled(levChar);
				return;
			}
			if (auto* levSpell = a_form->As<RE::TESLevSpell>()) {
				CollectLeveled(levSpell);
				return;
			}
			if (auto* formList = a_form->As<RE::BGSListForm>()) {
				for (auto* item : formList->forms) {
					Collect(item);
				}
				return;
			}
			if (auto* container = a_form->As<RE::TESObjectCONT>()) {
				CollectContainer(container);
				return;
			}

			// Target if it can hold keywords
			if (auto* keywordForm = skyrim_cast<RE::BGSKeywordForm*>(a_form)) {
				_targets.push_back({ a_form, keywordForm });
			}
		}

		void CollectLeveled(const RE::TESLeveledList* a_list)
		{
			if (!a_list) {
				return;
			}
			for (const auto& entry : a_list->entries) {
				Collect(entry.form);
			}
		}

		void CollectContainer(const RE::TESContainer* a_container)
		{
			if (!a_container || !a_container->containerObjects) {
				return;
			}
			for (std::uint32_t i = 0; i < a_container->numContainerObjects; ++i) {
				if (const auto* entry = a_container->containerObjects[i]) {
					Collect(entry->obj);
				}
			}
		}

		std::vector<KeywordTarget> _targets;
		std::unordered_set<const RE::TESForm*> _visited;
	};

	template <class T>
	std::size_t CollectRoots(RE::TESDataHandler* a_dataHandler, const RE::TESFile* a_file, KeywordTargetCollector& a_collector)
	{
		std::size_t count = 0;
		for (auto* form : a_dataHandler->GetFormArray<T>()) {
			if (!form || !IsRecordInFile(form, a_file)) {
				continue;
			}
			++count;
			a_collector.CollectRoot(form);
		}
		return count;
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

		KeywordTargetCollector collector;
		std::size_t outfits = 0, levItems = 0, levChars = 0, levSpells = 0, formLists = 0, containers = 0, actors = 0;

		for (const auto& pluginName : a_rule.plugins) {
			const auto* file = a_dataHandler->LookupModByName(pluginName);
			if (!file) {
				SKSE::log::warn("  plugin '{}' is not loaded, skipping", pluginName);
				continue;
			}

			outfits += CollectRoots<RE::BGSOutfit>(a_dataHandler, file, collector);
			levItems += CollectRoots<RE::TESLevItem>(a_dataHandler, file, collector);
			levChars += CollectRoots<RE::TESLevCharacter>(a_dataHandler, file, collector);
			levSpells += CollectRoots<RE::TESLevSpell>(a_dataHandler, file, collector);
			formLists += CollectRoots<RE::BGSListForm>(a_dataHandler, file, collector);
			containers += CollectRoots<RE::TESObjectCONT>(a_dataHandler, file, collector);
			actors += CollectRoots<RE::TESNPC>(a_dataHandler, file, collector);
		}

		SKSE::log::info("  collections: {} outfit(s), {} leveled item list(s), {} leveled actor list(s), {} leveled spell list(s), {} form list(s), {} container(s), {} actor base(s)",
			outfits, levItems, levChars, levSpells, formLists, containers, actors);

		std::size_t formsTouched = 0;
		std::size_t keywordsAdded = 0;
		std::map<RE::FormType, std::size_t> touchedByType;

		for (const auto& target : collector.Targets()) {
			bool touched = false;
			for (auto* keyword : keywords) {
				if (target.keywordForm->HasKeyword(keyword)) {
					continue;
				}
				if (target.keywordForm->AddKeyword(keyword)) {
					++keywordsAdded;
					touched = true;
					SKSE::log::debug("  + {} -> {:08X} [{}] '{}'",
						keyword->GetFormEditorID(),
						target.form->GetFormID(),
						RE::FormTypeToString(target.form->GetFormType()),
						target.form->GetName());
				}
			}
			if (touched) {
				++formsTouched;
				++touchedByType[target.form->GetFormType()];
			}
		}

		SKSE::log::info("  -> {} keyword-capable form(s) reachable, added {} keyword(s) to {} form(s)", collector.Targets().size(), keywordsAdded, formsTouched);

		for (const auto& [type, count] : touchedByType) {
			SKSE::log::info("      {}: {}", RE::FormTypeToString(type), count);
		}
	}

	void OnDataLoaded()
	{
		SKSE::log::info("Data loaded");

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