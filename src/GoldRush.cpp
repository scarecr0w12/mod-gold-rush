#include "Common.h"
#include "Chat.h"
#include "Config.h"
#include "Containers.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Tokenize.h"
#include "PlayerbotAI.h"
#include "RandomPlayerbotMgr.h"
#include "Playerbots.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
static char const* const GoldRushLogFilter = "module.goldrush";

struct GoldRushSite
{
    std::string ZoneLabel;
    std::string AreaLabel;
    uint32 ZoneId = 0;
    uint32 AreaId = 0;
    uint32 MapId = 0;
};

struct SpawnedNode
{
    ObjectGuid Guid;
    uint32 Entry = 0;
};

struct GoldRushTelemetry
{
    uint32 EventsStarted = 0;
    uint32 EventsCompleted = 0;
    uint32 NodesSpawned = 0;
    uint32 NodesRemoved = 0;
    uint32 AllianceBotsRouted = 0;
    uint32 HordeBotsRouted = 0;
    uint32 LastZoneId = 0;
    uint32 LastAreaId = 0;
    std::string LastLocation;
};

class GoldRushConfig
{
public:
    bool Enabled = true;
    bool VerboseLogging = false;
    uint32 MinIntervalMs = 120 * MINUTE * IN_MILLISECONDS;
    uint32 MaxIntervalMs = 240 * MINUTE * IN_MILLISECONDS;
    uint32 DurationMs = 20 * MINUTE * IN_MILLISECONDS;
    uint32 MinNodeCount = 15;
    uint32 MaxNodeCount = 25;
    uint32 BotsPerFaction = 6;
    uint32 BotPulseMs = 30 * IN_MILLISECONDS;
    float SpawnRadius = 25.0f;
    std::string NodeEntries;
    std::string ZonePool;
    std::string Blacklist;
    std::string StartMessage = "A seismic anomaly has exposed a massive vein of rich minerals in {}!";
    std::string EndMessage = "The Gold Rush in {} has been exhausted.";
    bool Debug = true;
};

static std::string Trim(std::string value)
{
    auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};

    auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static std::string Normalize(std::string_view value)
{
    std::string normalized(value);
    normalized = Trim(normalized);

    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c)
    {
        return std::tolower(c);
    });

    return normalized;
}

static bool ContainsWord(std::string const& text, std::string_view needle)
{
    return Normalize(text).find(Normalize(needle)) != std::string::npos;
}

static AreaTableEntry const* ResolveAreaEntry(std::string const& areaName)
{
    if (areaName.empty())
        return nullptr;

    std::string needle = Normalize(areaName);
    LocaleConstant locale = sWorld->GetDefaultDbcLocale();

    for (uint32 i = 0; i < sAreaTableStore.GetNumRows(); ++i)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(i);
        if (!area)
            continue;

        if (Normalize(area->area_name[locale]) == needle)
            return area;

        for (uint32 localeIndex = 0; localeIndex < TOTAL_LOCALES; ++localeIndex)
        {
            if (localeIndex == locale)
                continue;

            if (Normalize(area->area_name[localeIndex]) == needle)
                return area;
        }
    }

    return nullptr;
}

class GoldRushManager
{
public:
    void LoadConfig()
    {
        _config.Enabled = sConfigMgr->GetOption<bool>("GoldRush.Enable", true);
        _config.VerboseLogging = sConfigMgr->GetOption<bool>("GoldRush.VerboseLogging", false);
        _config.MinIntervalMs = sConfigMgr->GetOption<uint32>("GoldRush.MinIntervalMinutes", 120) * MINUTE * IN_MILLISECONDS;
        _config.MaxIntervalMs = sConfigMgr->GetOption<uint32>("GoldRush.MaxIntervalMinutes", 240) * MINUTE * IN_MILLISECONDS;
        _config.DurationMs = sConfigMgr->GetOption<uint32>("GoldRush.DurationMinutes", 20) * MINUTE * IN_MILLISECONDS;
        _config.MinNodeCount = sConfigMgr->GetOption<uint32>("GoldRush.MinNodes", 15);
        _config.MaxNodeCount = sConfigMgr->GetOption<uint32>("GoldRush.MaxNodes", 25);
        _config.BotsPerFaction = sConfigMgr->GetOption<uint32>("GoldRush.BotsPerFaction", 6);
        _config.BotPulseMs = sConfigMgr->GetOption<uint32>("GoldRush.BotPulseSeconds", 30) * IN_MILLISECONDS;
        _config.SpawnRadius = sConfigMgr->GetOption<float>("GoldRush.SpawnRadiusYards", 25.0f);
        _config.NodeEntries = sConfigMgr->GetOption<std::string>("GoldRush.NodeEntries", "191133;190176;190171;190172;189973");
        _config.ZonePool = sConfigMgr->GetOption<std::string>("GoldRush.ZonePool", "Un'Goro Crater|Fire Plume Ridge; Winterspring|Frostfire Hot Springs; Eastern Plaguelands|Terrorweb Tunnel; Sholazar Basin|The River's Heart");
        _config.Blacklist = sConfigMgr->GetOption<std::string>("GoldRush.Blacklist", "Stormwind City; Orgrimmar; Dalaran");
        _config.StartMessage = sConfigMgr->GetOption<std::string>("GoldRush.StartMessage", "A seismic anomaly has exposed a massive vein of rich minerals in {}!");
        _config.EndMessage = sConfigMgr->GetOption<std::string>("GoldRush.EndMessage", "The Gold Rush in {} has been exhausted.");
        _config.Debug = sConfigMgr->GetOption<bool>("GoldRush.Debug", true);

        _nodeEntries = BuildNodeEntries(_config.NodeEntries);
        _blockedZones = BuildBlockedZones(_config.Blacklist);
        _sites.clear();
        _sitesInitialized = false;
        _active = false;
        _currentSite = {};
        _hotspot = {};
        _spawnedNodes.clear();
        _routedBotIds.clear();
        _telemetry = {};
        _timeUntilNextEventMs = 0;
        _eventTimeRemainingMs = 0;
        _nextBotPulseMs = _config.BotPulseMs;

        if (!_config.Enabled)
        {
            LOG_INFO(GoldRushLogFilter, "Gold Rush module disabled by configuration.");
            return;
        }

        if (_nodeEntries.empty())
        {
            LOG_WARN(GoldRushLogFilter, "Gold Rush loaded but no valid node template entries were configured.");
            return;
        }

        if (_config.VerboseLogging)
            LOG_INFO(GoldRushLogFilter, "Gold Rush verbose logging enabled.");
    }

    void Update(uint32 diff)
    {
        if (!_config.Enabled || _nodeEntries.empty())
            return;

        if (!EnsureSitesInitialized())
            return;

        if (_active)
        {
            if (_eventTimeRemainingMs <= diff)
            {
                EndEvent();
                return;
            }

            _eventTimeRemainingMs -= diff;

            if (_nextBotPulseMs <= diff)
            {
                MaintainBotPressure();
                _nextBotPulseMs = _config.BotPulseMs;
            }
            else
            {
                _nextBotPulseMs -= diff;
            }

            return;
        }

        if (_timeUntilNextEventMs <= diff)
        {
            StartEvent();
            return;
        }

        _timeUntilNextEventMs -= diff;
    }

    bool ForceStart(Player* anchor, std::string const& locationFilter = {})
    {
        if (!_config.Enabled || _nodeEntries.empty())
            return false;

        if (!EnsureSitesInitialized())
            return false;

        GoldRushSite site = locationFilter.empty() ? BuildEligibleSiteFromPlayer(anchor) : ResolveSiteByName(locationFilter);

        if (!site.ZoneId && anchor)
            site = BuildEligibleSiteFromPlayer(anchor);

        if (!site.ZoneId || !anchor || !anchor->IsInWorld() || !anchor->GetMap())
            return false;

        return StartEvent(anchor, site);
    }

    bool ForceTestStart(Player* anchor)
    {
        if (!_config.Enabled || _nodeEntries.empty())
        {
            LOG_WARN(GoldRushLogFilter, "Gold Rush test start rejected: module disabled or no node templates configured.");
            return false;
        }

        if (!EnsureSitesInitialized())
            return false;

        if (!anchor || !anchor->IsInWorld() || !anchor->GetMap())
            anchor = FindAnyWorldPlayer();

        if (!anchor || !anchor->IsInWorld() || !anchor->GetMap())
        {
            LOG_WARN(GoldRushLogFilter, "Gold Rush test start rejected: no valid world player anchor was available.");
            return false;
        }

        GoldRushSite site = BuildEligibleSiteFromPlayer(anchor);
        if (!site.ZoneId)
        {
            LOG_WARN(GoldRushLogFilter, "Gold Rush test start rejected: {} is not in an eligible world zone.",
                anchor->GetName());
            return false;
        }

        _currentSite = site;
        _hotspot = WorldPosition(anchor->GetMapId(), anchor->GetPositionX(), anchor->GetPositionY(), anchor->GetPositionZ(), anchor->GetOrientation());

        bool spawnedNodes = SpawnHotspot(anchor);

        _active = true;
        _eventTimeRemainingMs = _config.DurationMs;
        _nextBotPulseMs = 0;
        ++_telemetry.EventsStarted;
        _telemetry.LastZoneId = _currentSite.ZoneId;
        _telemetry.LastAreaId = _currentSite.AreaId;
        _telemetry.LastLocation = BuildLocationText(_currentSite);

        if (_config.VerboseLogging)
        {
            LOG_INFO(GoldRushLogFilter, "Gold Rush test start anchored at map {} zone {} area {} by {}.",
                _currentSite.MapId, _currentSite.ZoneId, _currentSite.AreaId, anchor->GetName());
        }

        ChatHandler(nullptr).SendWorldText(Acore::StringFormat(_config.StartMessage, FormatLocationForAnnouncement(_currentSite, anchor)));

        if (_config.Debug)
        {
            LOG_INFO(GoldRushLogFilter, "Gold Rush test start triggered in {} for {} minute(s) with {} temporary node(s).",
                _telemetry.LastLocation,
                _config.DurationMs / (MINUTE * IN_MILLISECONDS),
                _spawnedNodes.size());
        }

        if (!spawnedNodes && _config.VerboseLogging)
            LOG_INFO(GoldRushLogFilter, "Gold Rush test start continued without spawned nodes; hotspot validation only.");

        RouteBots();
        MaintainBotPressure();
        return true;
    }

    Player* FindAnyWorldPlayer() const
    {
        std::shared_lock lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            if (player && player->IsInWorld() && player->GetMap())
                return player;
        }

        return nullptr;
    }

    bool ForceStop()
    {
        if (!_active)
            return false;

        EndEvent(true);
        return true;
    }

    std::string GetStatusText() const
    {
        std::ostringstream out;
        out << "Gold Rush: ";
        out << (_active ? "ACTIVE" : "IDLE");
        if (_active)
        {
            out << " | Location: " << _telemetry.LastLocation;
            out << " | Remaining: " << (_eventTimeRemainingMs / IN_MILLISECONDS) << "s";
            out << " | Nodes: " << _spawnedNodes.size();
            out << " | Routed A/H: " << _telemetry.AllianceBotsRouted << "/" << _telemetry.HordeBotsRouted;
        }
        out << " | Events started/completed: " << _telemetry.EventsStarted << "/" << _telemetry.EventsCompleted;
        out << " | Nodes spawned/removed: " << _telemetry.NodesSpawned << "/" << _telemetry.NodesRemoved;
        return out.str();
    }

    void RefreshBotPressure()
    {
        MaintainBotPressure();
    }

private:
    bool EnsureSitesInitialized()
    {
        if (_sitesInitialized)
            return true;

        _sites = BuildSites(_config.ZonePool);
        _sitesInitialized = true;
        _timeUntilNextEventMs = RollIntervalMs();

        if (_sites.empty())
        {
            LOG_WARN(GoldRushLogFilter,
                "Gold Rush loaded without configured hotspot zones; falling back to eligible live-player zones.");
            return true;
        }

        LOG_INFO(GoldRushLogFilter, "Gold Rush loaded with {} hotspot(s), {} node template(s), interval {}-{} minute(s), duration {} minute(s).",
            _sites.size(),
            _nodeEntries.size(),
            _config.MinIntervalMs / (MINUTE * IN_MILLISECONDS),
            _config.MaxIntervalMs / (MINUTE * IN_MILLISECONDS),
            _config.DurationMs / (MINUTE * IN_MILLISECONDS));

        return true;
    }

    static std::unordered_set<std::string> BuildBlockedZones(std::string const& blacklist)
    {
        std::unordered_set<std::string> blocked;
        for (std::string_view blockedZone : Acore::Tokenize(blacklist, ';', true))
        {
            std::string normalized = Normalize(blockedZone);
            if (!normalized.empty())
                blocked.insert(std::move(normalized));
        }

        return blocked;
    }

    static bool IsValidZoneArea(AreaTableEntry const* zoneArea)
    {
        if (!zoneArea)
            return false;

        if (zoneArea->IsSanctuary())
            return false;

        uint32 flags = zoneArea->flags;
        return (flags & (AREA_FLAG_CAPITAL | AREA_FLAG_CITY | AREA_FLAG_SLAVE_CAPITAL)) == 0;
    }

    bool IsBlockedSite(GoldRushSite const& site) const
    {
        std::string zoneKey = Normalize(site.ZoneLabel.empty() ? GetAreaLabelById(site.ZoneId) : site.ZoneLabel);
        std::string areaKey = Normalize(site.AreaLabel);
        std::string fullKey = areaKey.empty() ? zoneKey : Acore::StringFormat("{} near {}", zoneKey, areaKey);

        return _blockedZones.find(zoneKey) != _blockedZones.end() ||
            (!areaKey.empty() && _blockedZones.find(fullKey) != _blockedZones.end());
    }

    bool IsEligibleAnchor(Player const* player) const
    {
        if (!player || !player->IsInWorld() || !player->GetMap())
            return false;

        MapEntry const* mapEntry = player->GetMap()->GetEntry();
        if (!mapEntry || !mapEntry->IsWorldMap())
            return false;

        GoldRushSite site = BuildSiteFromAnchor(player);
        if (!site.ZoneId)
            return false;

        AreaTableEntry const* zoneArea = sAreaTableStore.LookupEntry(site.ZoneId);
        if (!IsValidZoneArea(zoneArea))
            return false;

        return !IsBlockedSite(site);
    }

    GoldRushSite BuildEligibleSiteFromPlayer(Player const* player) const
    {
        if (!IsEligibleAnchor(player))
            return {};

        return BuildSiteFromAnchor(player);
    }

    std::vector<GoldRushSite> BuildSites(std::string const& zones) const
    {

        std::vector<GoldRushSite> sites;
        for (std::string_view zoneToken : Acore::Tokenize(zones, ';', true))
        {
            std::string token = Trim(std::string(zoneToken));
            if (token.empty())
                continue;

            std::vector<std::string_view> parts = Acore::Tokenize(token, '|', true);

            GoldRushSite site;
            site.ZoneLabel = parts.empty() ? token : Trim(std::string(parts[0]));
            if (parts.size() > 1)
                site.AreaLabel = Trim(std::string(parts[1]));

            if (site.ZoneLabel.empty())
                continue;

            AreaTableEntry const* zoneArea = ResolveAreaEntry(site.ZoneLabel);
            AreaTableEntry const* areaEntry = site.AreaLabel.empty() ? zoneArea : ResolveAreaEntry(site.AreaLabel);
            if (!zoneArea)
                continue;

            AreaTableEntry const* rootZone = zoneArea->zone ? sAreaTableStore.LookupEntry(zoneArea->zone) : zoneArea;
            if (!rootZone)
                continue;

            if (!site.AreaLabel.empty() && !areaEntry)
                continue;

            site.ZoneId = rootZone->ID;
            site.AreaId = !site.AreaLabel.empty() && areaEntry ? areaEntry->ID : 0;
            site.MapId = rootZone->mapid;

            if (!IsValidZoneArea(rootZone) || IsBlockedSite(site))
                continue;

            sites.push_back(std::move(site));
        }

        return sites;
    }

    static std::vector<uint32> BuildNodeEntries(std::string const& entries)
    {
        std::vector<uint32> nodeEntries;
        for (std::string_view token : Acore::Tokenize(entries, ';', true))
        {
            std::string trimmed = Trim(std::string(token));
            if (trimmed.empty())
                continue;

            if (Optional<uint32> entry = Acore::StringTo<uint32>(trimmed); entry && *entry)
                nodeEntries.push_back(*entry);
        }

        return nodeEntries;
    }

    void LogVerbose(std::string const& message) const
    {
        if (_config.VerboseLogging)
            LOG_INFO(GoldRushLogFilter, "{}", message);
    }

    static std::string BuildLocationText(GoldRushSite const& site)
    {
        if (site.AreaLabel.empty())
            return site.ZoneLabel;

        return Acore::StringFormat("{} near {}", site.ZoneLabel, site.AreaLabel);
    }

    static std::string FormatLocationForAnnouncement(GoldRushSite const& site, Player const* anchor)
    {
        if (anchor && anchor->GetZoneId() == site.ZoneId)
        {
            if (!site.AreaLabel.empty())
                return Acore::StringFormat("{} near {}", site.ZoneLabel, site.AreaLabel);

            return site.ZoneLabel;
        }

        return BuildLocationText(site);
    }

    uint32 RollIntervalMs() const
    {
        uint32 minMs = std::min(_config.MinIntervalMs, _config.MaxIntervalMs);
        uint32 maxMs = std::max(_config.MinIntervalMs, _config.MaxIntervalMs);

        if (maxMs <= minMs)
            return minMs;

        return RandomValue(minMs, maxMs);
    }

    static uint32 RandomValue(uint32 minValue, uint32 maxValue)
    {
        static std::mt19937 rng{ std::random_device{}() };
        std::uniform_int_distribution<uint32> distribution(minValue, maxValue);
        return distribution(rng);
    }

    static float RandomFloat(float minValue, float maxValue)
    {
        static std::mt19937 rng{ std::random_device{}() };
        std::uniform_real_distribution<float> distribution(minValue, maxValue);
        return distribution(rng);
    }

    static bool IsHotspotNodeEntry(uint32 entry)
    {
        GameObjectTemplate const* goinfo = sObjectMgr->GetGameObjectTemplate(entry);
        if (!goinfo)
            return false;

        std::string name = Normalize(goinfo->name);
        return ContainsWord(name, "vein") || ContainsWord(name, "lotus") ||
               ContainsWord(name, "clover") || ContainsWord(name, "thorn") ||
               ContainsWord(name, "bloom");
    }

    static std::vector<uint32> SplitByCategory(std::vector<uint32> const& entries, bool herbs)
    {
        std::vector<uint32> result;
        for (uint32 entry : entries)
        {
            if (!entry)
                continue;

            GameObjectTemplate const* goinfo = sObjectMgr->GetGameObjectTemplate(entry);
            if (!goinfo)
                continue;

            std::string name = Normalize(goinfo->name);
            bool isHerb = ContainsWord(name, "lotus") || ContainsWord(name, "clover") || ContainsWord(name, "thorn") || ContainsWord(name, "bloom");
            bool isOre = ContainsWord(name, "vein");

            if (herbs && isHerb)
                result.push_back(entry);
            else if (!herbs && isOre)
                result.push_back(entry);
        }

        return result;
    }

    GoldRushSite ResolveSiteByName(std::string const& name) const
    {
        std::string needle = Normalize(name);
        for (GoldRushSite const& site : _sites)
        {
            if (Normalize(site.ZoneLabel) == needle || Normalize(site.AreaLabel) == needle ||
                Normalize(BuildLocationText(site)) == needle)
            {
                return site;
            }
        }

        return {};
    }

    static GoldRushSite BuildSiteFromAnchor(Player const* anchor)
    {
        GoldRushSite site;
        if (!anchor)
            return site;

        site.ZoneId = anchor->GetZoneId();
        site.AreaId = anchor->GetAreaId();
        site.MapId = anchor->GetMapId();
        site.ZoneLabel = GetAreaLabelById(site.ZoneId);
        site.AreaLabel = site.AreaId ? GetAreaLabelById(site.AreaId) : std::string();
        return site;
    }

    static std::string GetAreaLabelById(uint32 areaId)
    {
        if (!areaId)
            return {};

        LocaleConstant locale = sWorld->GetDefaultDbcLocale();
        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId))
            return area->area_name[locale];

        return {};
    }

    GoldRushSite SelectSiteForPlayer(Player const* player) const
    {
        GoldRushSite playerSite = BuildEligibleSiteFromPlayer(player);
        if (playerSite.ZoneId)
            return playerSite;

        if (!_sites.empty())
            return Acore::Containers::SelectRandomContainerElement(_sites);

        if (Player* fallback = FindAnyEligibleWorldPlayer())
            return BuildEligibleSiteFromPlayer(fallback);

        return {};
    }

    Player* FindAnchorPlayer(GoldRushSite const& site) const
    {
        std::shared_lock lock(*HashMapHolder<Player>::GetLock());
        Player* zoneFallback = nullptr;

        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            if (!IsEligibleAnchor(player))
                continue;

            if (site.MapId && player->GetMapId() != site.MapId)
                continue;

            if (site.ZoneId && player->GetZoneId() != site.ZoneId)
                continue;

            if (site.AreaId)
            {
                if (player->GetAreaId() == site.AreaId)
                    return player;

                if (!zoneFallback)
                    zoneFallback = player;

                continue;
            }

            return player;
        }

        return zoneFallback;
    }

    Player* FindAnyEligibleWorldPlayer() const
    {
        std::shared_lock lock(*HashMapHolder<Player>::GetLock());
        std::vector<Player*> candidates;

        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            if (IsEligibleAnchor(player))
                candidates.push_back(player);
        }

        if (candidates.empty())
            return nullptr;

        return Acore::Containers::SelectRandomContainerElement(candidates);
    }

    bool StartEvent(Player* anchorOverride = nullptr, GoldRushSite siteOverride = {})
    {
        if (_nodeEntries.empty())
            return false;

        if (!siteOverride.ZoneId)
            siteOverride = anchorOverride ? BuildEligibleSiteFromPlayer(anchorOverride) : SelectSiteForPlayer(anchorOverride);

        if (!siteOverride.ZoneId)
            siteOverride = SelectSiteForPlayer(anchorOverride);

        if (!siteOverride.ZoneId)
        {
            if (Player* dynamicAnchor = FindAnyEligibleWorldPlayer())
            {
                anchorOverride = dynamicAnchor;
                siteOverride = BuildEligibleSiteFromPlayer(dynamicAnchor);
            }
        }

        if (!siteOverride.ZoneId)
        {
            _timeUntilNextEventMs = 5 * MINUTE * IN_MILLISECONDS;
            if (_config.Debug)
                LOG_INFO(GoldRushLogFilter, "Gold Rush could not find any eligible world zone with a live player anchor; retrying in 5 minutes.");
            return false;
        }

        _currentSite = siteOverride;

        Player* anchor = anchorOverride && IsEligibleAnchor(anchorOverride) ? anchorOverride : FindAnchorPlayer(_currentSite);
        if (!anchor || !anchor->GetMap())
        {
            anchor = FindAnyEligibleWorldPlayer();
            if (!anchor || !anchor->GetMap())
            {
                _timeUntilNextEventMs = 5 * MINUTE * IN_MILLISECONDS;
                if (_config.Debug)
                    LOG_INFO(GoldRushLogFilter, "Gold Rush hotspot in {} could not find an anchor player; retrying in 5 minutes.", BuildLocationText(_currentSite));
                return false;
            }
        }

        GoldRushSite anchorSite = BuildEligibleSiteFromPlayer(anchor);
        if (!anchorSite.ZoneId)
        {
            _timeUntilNextEventMs = 5 * MINUTE * IN_MILLISECONDS;
            if (_config.Debug)
                LOG_INFO(GoldRushLogFilter, "Gold Rush rejected anchor {} because the zone is no longer eligible; retrying in 5 minutes.",
                    anchor->GetName());
            return false;
        }

        _currentSite = anchorSite;

        _hotspot = WorldPosition(anchor->GetMapId(), anchor->GetPositionX(), anchor->GetPositionY(), anchor->GetPositionZ(), anchor->GetOrientation());

        if (!SpawnHotspot(anchor))
        {
            _timeUntilNextEventMs = 5 * MINUTE * IN_MILLISECONDS;
            if (_config.Debug)
                LOG_INFO(GoldRushLogFilter, "Gold Rush hotspot in {} could not spawn nodes; retrying in 5 minutes.", BuildLocationText(_currentSite));
            return false;
        }

        _active = true;
        _eventTimeRemainingMs = _config.DurationMs;
        _nextBotPulseMs = 0;
        ++_telemetry.EventsStarted;
        _telemetry.LastZoneId = _currentSite.ZoneId;
        _telemetry.LastAreaId = _currentSite.AreaId;
        _telemetry.LastLocation = BuildLocationText(_currentSite);

        if (_config.VerboseLogging)
        {
            LOG_INFO(GoldRushLogFilter, "Gold Rush event anchored at map {} zone {} area {} by {}.",
                _currentSite.MapId, _currentSite.ZoneId, _currentSite.AreaId, anchor->GetName());
        }

        std::string announcementLocation = FormatLocationForAnnouncement(_currentSite, anchor);
        ChatHandler(nullptr).SendWorldText(Acore::StringFormat(_config.StartMessage, announcementLocation));

        if (_config.Debug)
        {
            LOG_INFO(GoldRushLogFilter, "Gold Rush started in {} for {} minute(s) with {} temporary node(s).",
                _telemetry.LastLocation,
                _config.DurationMs / (MINUTE * IN_MILLISECONDS),
                _spawnedNodes.size());
        }

        RouteBots();
        MaintainBotPressure();
        return true;
    }

    void EndEvent(bool forced = false)
    {
        if (!_active)
            return;

        std::string location = BuildLocationText(_currentSite);
        ChatHandler(nullptr).SendWorldText(Acore::StringFormat(_config.EndMessage, location));

        if (_config.Debug)
            LOG_INFO(GoldRushLogFilter, "Gold Rush ended in {}{}.", location, forced ? " (forced)" : "");

        DespawnHotspotNodes();
        ClearBotRouting();

        _active = false;
        _currentSite = {};
        _hotspot = WorldPosition();
        _eventTimeRemainingMs = 0;
        _timeUntilNextEventMs = RollIntervalMs();
        _nextBotPulseMs = _config.BotPulseMs;
        ++_telemetry.EventsCompleted;
    }

    bool SpawnHotspot(Player* anchor)
    {
        if (!anchor || !anchor->GetMap())
            return false;

        Map* map = anchor->GetMap();
        uint32 minNodes = std::min(_config.MinNodeCount, _config.MaxNodeCount);
        uint32 maxNodes = std::max(_config.MinNodeCount, _config.MaxNodeCount);
        uint32 nodeCount = RandomValue(minNodes, maxNodes);
        nodeCount = std::max<uint32>(1, nodeCount);

        std::vector<uint32> oreEntries = SplitByCategory(_nodeEntries, false);
        std::vector<uint32> herbEntries = SplitByCategory(_nodeEntries, true);
        std::vector<uint32> fallbackEntries = _nodeEntries;
        bool useHerb = true;

        if (_config.VerboseLogging)
        {
            LOG_INFO(GoldRushLogFilter, "Gold Rush spawning {} node(s) using {} ore entry(s), {} herb entry(s), and {} fallback entry(s).",
                nodeCount, oreEntries.size(), herbEntries.size(), fallbackEntries.size());
        }

        constexpr float twoPi = 6.28318530717958647692f;
        for (uint32 i = 0; i < nodeCount; ++i)
        {
            std::vector<uint32> const* pool = nullptr;
            if (useHerb && !herbEntries.empty())
                pool = &herbEntries;
            else if (!useHerb && !oreEntries.empty())
                pool = &oreEntries;
            else
                pool = &fallbackEntries;

            useHerb = !useHerb;

            if (!pool || pool->empty())
                continue;

            uint32 entry = Acore::Containers::SelectRandomContainerElement(*pool);
            float angle = RandomFloat(0.0f, twoPi);
            float distance = RandomFloat(0.0f, _config.SpawnRadius);
            float x = anchor->GetPositionX() + (std::cos(angle) * distance);
            float y = anchor->GetPositionY() + (std::sin(angle) * distance);
            float z = anchor->GetPositionZ();

            if (GameObject* go = map->SummonGameObject(entry, x, y, z, anchor->GetOrientation(), 0.0f, 0.0f, 0.0f, 0.0f, _config.DurationMs, true))
            {
                go->SetRespawnTime(_config.DurationMs);
                go->SetSpawnedByDefault(false);
                _spawnedNodes.push_back({ go->GetGUID(), entry });
                ++_telemetry.NodesSpawned;

                if (_config.VerboseLogging)
                {
                    LOG_INFO(GoldRushLogFilter, "Gold Rush node spawned: entry {} at ({:.2f}, {:.2f}, {:.2f}) on map {}.",
                        entry, x, y, z, map->GetId());
                }
            }
        }

        return !_spawnedNodes.empty();
    }

    void DespawnHotspotNodes()
    {
        for (SpawnedNode const& node : _spawnedNodes)
        {
            if (!node.Guid)
                continue;

            if (Map* map = sMapMgr->FindMap(_currentSite.MapId, 0))
            {
                if (GameObject* go = map->GetGameObject(node.Guid))
                {
                    go->DespawnOrUnsummon();
                    ++_telemetry.NodesRemoved;
                }
            }
        }

        _spawnedNodes.clear();
    }

    void RouteBots()
    {
        if (!_active)
            return;

        std::vector<Player*> candidates = sRandomPlayerbotMgr.GetPlayers();
        std::vector<Player*> alliance;
        std::vector<Player*> horde;

        for (Player* bot : candidates)
        {
            if (!bot || !bot->IsInWorld() || bot->GetLevel() < 80 || !sRandomPlayerbotMgr.IsRandomBot(bot))
                continue;

            if (bot->GetTeamId() == TEAM_ALLIANCE)
                alliance.push_back(bot);
            else
                horde.push_back(bot);
        }

        RouteFactionBots(alliance, TEAM_ALLIANCE);
        RouteFactionBots(horde, TEAM_HORDE);

        if (_config.VerboseLogging)
        {
            LOG_INFO(GoldRushLogFilter, "Gold Rush bot routing scan complete: {} alliance candidate(s), {} horde candidate(s), {} routed bot(s) total.",
                alliance.size(), horde.size(), _routedBotIds.size());
        }
    }

    void RouteFactionBots(std::vector<Player*>& bots, TeamId team)
    {
        if (bots.empty())
            return;

        std::shuffle(bots.begin(), bots.end(), std::mt19937{ std::random_device{}() });

        uint32 targetCount = std::min<uint32>(_config.BotsPerFaction, bots.size());
        uint32 routedCount = 0;

        for (Player* bot : bots)
        {
            if (routedCount >= targetCount)
                break;

            if (!bot || _routedBotIds.find(bot->GetGUID().GetCounter()) != _routedBotIds.end())
                continue;

            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            {
                WorldPosition pos(_hotspot.GetMapId(), _hotspot.GetPositionX(), _hotspot.GetPositionY(), _hotspot.GetPositionZ());
                botAI->rpgInfo.ChangeToGoGrind(pos);
                botAI->ChangeStrategy("new rpg", BOT_STATE_NON_COMBAT);
                botAI->ChangeStrategy("new rpg", BOT_STATE_COMBAT);
                _routedBotIds.insert(bot->GetGUID().GetCounter());
                ++routedCount;

                if (team == TEAM_ALLIANCE)
                    ++_telemetry.AllianceBotsRouted;
                else
                    ++_telemetry.HordeBotsRouted;

                if (_config.VerboseLogging)
                {
                    LOG_INFO(GoldRushLogFilter, "Gold Rush routed {} bot {} toward {}.",
                        team == TEAM_ALLIANCE ? "Alliance" : "Horde",
                        bot->GetName(),
                        _telemetry.LastLocation);
                }
            }
        }
    }

    void MaintainBotPressure()
    {
        if (!_active)
            return;

        uint32 allianceCount = 0;
        uint32 hordeCount = 0;

        for (ObjectGuid::LowType guid : _routedBotIds)
        {
            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
            if (!bot || !bot->IsInWorld())
                continue;

            if (bot->GetTeamId() == TEAM_ALLIANCE)
                ++allianceCount;
            else
                ++hordeCount;
        }

        if (allianceCount < _config.BotsPerFaction || hordeCount < _config.BotsPerFaction)
        {
            if (_config.Debug)
                LOG_INFO(GoldRushLogFilter, "Gold Rush bot pressure low: alliance {}, horde {}. Recruiting reinforcements.", allianceCount, hordeCount);

            RouteBots();
        }
    }

    void ClearBotRouting()
    {
        for (ObjectGuid::LowType guid : _routedBotIds)
        {
            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
            if (!bot)
                continue;

            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                botAI->rpgInfo.ChangeToIdle();

            if (_config.VerboseLogging)
                LOG_INFO(GoldRushLogFilter, "Gold Rush released routed bot {} back to idle.", bot->GetName());
        }

        _routedBotIds.clear();
    }

    std::vector<GoldRushSite> const& GetSites() const { return _sites; }

    GoldRushConfig _config;
    GoldRushTelemetry _telemetry;
    std::unordered_set<std::string> _blockedZones;
    std::vector<GoldRushSite> _sites;
    std::vector<uint32> _nodeEntries;
    GoldRushSite _currentSite;
    WorldPosition _hotspot;
    std::vector<SpawnedNode> _spawnedNodes;
    std::unordered_set<ObjectGuid::LowType> _routedBotIds;
    uint32 _timeUntilNextEventMs = 0;
    uint32 _eventTimeRemainingMs = 0;
    uint32 _nextBotPulseMs = 0;
    bool _sitesInitialized = false;
    bool _active = false;
};

GoldRushManager g_GoldRushManager;

class GoldRushWorldScript : public WorldScript
{
public:
    GoldRushWorldScript() : WorldScript("GoldRushWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_GoldRushManager.LoadConfig();
    }

    void OnUpdate(uint32 diff) override
    {
        g_GoldRushManager.Update(diff);
    }
};

class GoldRushCommandScript : public CommandScript
{
public:
    GoldRushCommandScript() : CommandScript("GoldRushCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable goldRushCommandTable = {
            { "status", HandleStatusCommand, SEC_GAMEMASTER, Console::Yes },
            { "start", HandleStartCommand, SEC_GAMEMASTER, Console::No },
            { "teststart", HandleTestStartCommand, SEC_GAMEMASTER, Console::No },
            { "stop", HandleStopCommand, SEC_GAMEMASTER, Console::Yes },
            { "reroute", HandleRerouteCommand, SEC_GAMEMASTER, Console::No },
        };

        static ChatCommandTable commandTable = {
            { "goldrush", goldRushCommandTable },
        };

        return commandTable;
    }

    static bool HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
    {
        handler->PSendSysMessage("{}", g_GoldRushManager.GetStatusText());
        return true;
    }

    static bool HandleStartCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        std::string location = args ? Trim(args) : std::string();

        if (g_GoldRushManager.ForceStart(player, location))
        {
            handler->PSendSysMessage("Gold Rush started.");
            return true;
        }

        handler->PSendSysMessage("Gold Rush could not be started. Make sure you are in an eligible world zone or name a configured hotspot.");
        return false;
    }

    static bool HandleTestStartCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;

        LOG_INFO(GoldRushLogFilter, "Gold Rush teststart command received from {}.", player ? player->GetName() : "console");

        if (g_GoldRushManager.ForceTestStart(player))
        {
            handler->PSendSysMessage("Gold Rush test start triggered at your current location.");
            return true;
        }

        LOG_WARN(GoldRushLogFilter, "Gold Rush teststart command failed for {}.", player ? player->GetName() : "console");
        handler->PSendSysMessage("Gold Rush test start could not be triggered. Make sure you are in an eligible world zone.");
        return false;
    }

    static bool HandleStopCommand(ChatHandler* handler, char const* /*args*/)
    {
        if (g_GoldRushManager.ForceStop())
        {
            handler->PSendSysMessage("Gold Rush stopped.");
            return true;
        }

        handler->PSendSysMessage("Gold Rush is not active.");
        return false;
    }

    static bool HandleRerouteCommand(ChatHandler* handler, char const* /*args*/)
    {
        g_GoldRushManager.RefreshBotPressure();
        handler->PSendSysMessage("Gold Rush bot routing refreshed.");
        return true;
    }
};
} // namespace

void AddSC_mod_gold_rush()
{
    new GoldRushWorldScript();
    new GoldRushCommandScript();
}
