#include "Utils.hh"
#include "injector/utility.hpp"
#include <cstring>
#include <CTheScripts.hh>
#include <gtaThread.hh>
#include <unordered_map>
#include <random>
#include <ctime>
#include <eStats.hh>
#include <CMaths.hh>
#include "logger.hh"
#include "common.hh"
#include "config.hh"
#include <array>
#include <CCrypto.hh>
#include <eGlobalVariables.hh>

int (__thiscall *scrThread__RunOriginal) (scrThread *scr, unsigned int param_2);

struct MissionInfo
{
    int     strand;
    Vector3 endPos;
    int     citiesUnlocked;
    int     citiesUnlockedStart;
    bool    phoneMission;
    Vector3 startPos;
    int     stackSize;
};

std::unordered_map<std::string, MissionInfo> mMissionInfos{};

struct MissionStrandInfo
{
    int __unk00[6];
    int m_nLastMissionPassed;
    int __unk28[77];
};

static_assert (sizeof (MissionStrandInfo) == 4 * 84,
               "MissionStrandInfo is wrong size");

struct PreviousChange
{
    uint32_t hash = -1;
    uint32_t offset;
    uint8_t  originalValue;
    uint8_t  newValue;
};

class MissionRandomizer
{
    static std::unordered_map<std::string, std::string> mMissionMaps;
    static MissionInfo                                  mRandomizedMission;
    static MissionInfo                                  mOriginalMission;
    static int                                          mStoredIslandsUnlocked;
    static uint32_t                                     mMissionHash;
    static uint32_t                                     mOriginalMissionHash;
    static PreviousChange                               mPreviousChange;
    static int                                          mStoredBohanHouseState;
    static int                                          mStoredMobilePhone;
    static uint32_t                                     mCurrentMissionSeed;

    // Related to teleportation
    static bool    mbFading;
    static bool    mbFadeInAfterTeleport;
    static int     miFadeTimer;
    static Vector3 mvPosAfterFade;

    /*******************************************************/
    static void
    InitialiseStoredSeed ()
    {
        uint32_t seed
            = CNativeManager::CallNativeRet<uint32_t> ("GET_FLOAT_STAT", 252);

        if (seed == mCurrentMissionSeed && seed != 0)
            return;
        
        if (seed == 0)
            {
                seed = RandomUInt(UINT_MAX);
                CNativeManager::CallNative ("SET_FLOAT_STAT", 252, seed);
            }
        else
            Rainbomizer::Logger::LogMessage ("Seeding from save file: %u", seed);
        
        InitialiseMissionsMap (seed);
    }

    /*******************************************************/
    static std::string
    GetNewMission (std::string old)
    {
        auto &config = ConfigManager::GetConfigs ().missions;
        std::transform (old.begin (), old.end (), old.begin (),
                        [] (unsigned char c) { return std::tolower (c); });

        if (mMissionMaps.count (old))
            {
                InitialiseStoredSeed ();
                if (config.forcedMissionEnabled)
                    if (mMissionMaps.count (config.forcedMissionID))
                        return config.forcedMissionID;

                return mMissionMaps[old];
            }
        return old;
    }

    // Macro so I don't have to repeat similar code multiple times
#define ADJUST_SCRIPT_NAME(name, native)                                         \
    static void name (NativeData *data)                                          \
    {                                                                            \
        std::string newThread      = GetNewMission (data->Params[0].cstr_param); \
        data->Params[0].cstr_param = newThread.data ();                          \
                                                                                 \
        CNativeManager::CallOriginalNative (native, data);                       \
    }

    ADJUST_SCRIPT_NAME (HasScriptLoaded, "HAS_SCRIPT_LOADED")
    ADJUST_SCRIPT_NAME (RequestScript, "REQUEST_SCRIPT")
    ADJUST_SCRIPT_NAME (MarkScriptAsNoLongerNeeded,
                        "MARK_SCRIPT_AS_NO_LONGER_NEEDED")

#undef ADJUST_SCRIPT_NAME

    /*******************************************************/
    // This function adjusts the strand the mission will increment after a
    // mission passes. It changes the ipush1 instruction in the mission pass
    // strand code function.
    /*******************************************************/
    static void
    AdjustMissionPassCode (scrProgram *program)
    {
        auto pattern = hook::make_range_pattern (
            (uintptr_t) program->m_pCodeBlock,
            (uintptr_t) program->m_pCodeBlock + program->m_nCodeSize,
            "2f 03 06 00 ? 3b 32 3b 31 36 31 37 31 38 31 43 ?");

        pattern.for_each_result ([] (hook::pattern_match match) {
            *match.get<char> (4) = mOriginalMission.strand + 96;
        });
    }

    /*******************************************************/
    // This function adjusts the strand for the mission fail process code. If
    // this is not corrected, mission replay will not be activated. Further,
    // this code also adjusts the mission passes for missions like Uncle Vlad,
    // which deactivate a strand when you pass it. If this fucntion detects such
    // a code, the code will put a "return 0x0" instruction in-place of the
    // ipush1 instruction to prevent any stack corruptions.
    /*******************************************************/
    static void
    AdjustMissionFailCode (scrProgram *program)
    {
        auto pattern
            = hook::make_range_pattern ((uintptr_t) program->m_pCodeBlock,
                                        (uintptr_t) program->m_pCodeBlock
                                            + program->m_nCodeSize,
                                        "2f 00 03 00 ? 38 32 38 31 22");

        if (mPreviousChange.hash == program->m_dwHash
            && program->m_pCodeBlock[mPreviousChange.offset]
                   == mPreviousChange.newValue)
            return;

        pattern.for_each_result ([&program] (hook::pattern_match match) {
            uint8_t &strand = *match.get<uint8_t> (4);

            // ipush1 <strand>
            if (strand - 96 == mRandomizedMission.strand)
                {
                    strand = mOriginalMission.strand + 96;

                    if (strand - 96 != mRandomizedMission.strand)
                        {
                            mPreviousChange.hash = program->m_dwHash;
                            mPreviousChange.offset
                                = &strand - program->m_pCodeBlock;
                            mPreviousChange.newValue = strand;
                        }

                    return;
                }

            // ret 0x0
            *match.get<uint8_t> (4)  = 0x30;
            *match.get<uint16_t> (5) = 0;
        });
    }

    /*******************************************************/
    static void
    SetRomansApartmentState (bool isBurned)
    {
        static const std::array buildings{
            // Safehouses
            std::make_tuple (896.52f, -502.84f, 19.48f, 175.0f,
                             "rt4_she_ksun"_joaat, "rt4_she_fire_ksun"_joaat),
            std::make_tuple (896.52f, -502.84f, 19.48f, 175.0f,
                             "lodrt4_she_ksun"_joaat, "lodrt4fir_sheks"_joaat),

            // Cab Depot
            std::make_tuple (822.56f, -270.34f, 15.6157f, 75.0f,
                             "rh5a_ksun"_joaat, "rh5aburnt_ksun"_joaat),
            std::make_tuple (822.5629f, -270.342f, 15.6157f, 75.0f,
                             "lod_rh5a2_ksun"_joaat,
                             "lod_rh5aburnt_ksun"_joaat)};

        for (auto i : buildings)
            {
                const auto &[x, y, z, r, normal, burned] = i;
                CNativeManager::CallNative ("SWAP_NEAREST_BUILDING_MODEL", x, y,
                                            z, r, (isBurned) ? normal : burned,
                                            (isBurned) ? burned : normal);

                CTheScripts::m_pGlobals ()[ROMAN_APARTMENT_BURNED_DOWN]
                    = isBurned;
            }
    }

    /*******************************************************/
    static void
    SetClubhouseState (bool unlocked)
    {
        //Unlock doors

        static_assert("cj_ext_door_11"_joaat == 807349477u);
        
        static const std::array doors{
            std::make_tuple (1643309849u, -1715.0f, 354.0f, 26.0f),
            std::make_tuple (1643309849u, -1726.0f, 342.0f, 27.0f),
            std::make_tuple ("cj_ext_door_11"_joaat, -1730.0f, 326.0f, 43.0f)
        };

        for (const auto &i : doors)
            {
                const auto &[hash, x, y, z] = i;
                CNativeManager::CallNative ("SET_STATE_OF_CLOSEST_DOOR_OF_TYPE",
                                            hash, x, y, z, (int) !unlocked, 0.0f);
            }

        // Swap models
        static const std::array buildings{
            std::make_tuple (-1716.52f, 362.84f, 27.48f, 75.0f,
                             "nj03ac3200"_joaat, 929285946u),
            std::make_tuple (-1716.52f, 362.84f, 27.48f, 75.0f,
                             "lod3ac3200"_joaat, 1790619695u),
        };

        for (const auto &i: buildings)
            {
                const auto &[x, y, z, r, normal, burned] = i;
                CNativeManager::CallNative ("SWAP_NEAREST_BUILDING_MODEL", x, y,
                                            z, r, (unlocked) ? burned : normal,
                                            (unlocked) ? normal : burned);
            }

        // Enable save house
        CTheScripts::m_pGlobals ()[CLUBHOUSE_OPEN] = unlocked;
        CNativeManager::CallNative ("ENABLE_SAVE_HOUSE",
                                    CTheScripts::m_pGlobals ()[CLUBHOUSE_INDEX],
                                    unlocked);
    }

    /*******************************************************/
    static inline bool
    ShouldFixCabDepot ()
    {
        return mRandomizedMission.citiesUnlocked == 1
               && mOriginalMissionHash != "roman11"_joaat;
    }

    /*******************************************************/
    static void
    HandleRomansSorrowChanges (bool missionEnd)
    {
        if (Rainbomizer::Common::GetStoredEpisodeNumber () != 0)
            return;

        static bool revertDepot     = false;
        static bool depotBurnedDown = false;

        if (!missionEnd && ShouldFixCabDepot ())
            {
                revertDepot = true;
                depotBurnedDown
                    = CTheScripts::m_pGlobals ()[ROMAN_APARTMENT_BURNED_DOWN];
                SetRomansApartmentState (false);
            }

        if (missionEnd && revertDepot)
            {
                SetRomansApartmentState (depotBurnedDown);
                revertDepot = false;
            }
    }

    /*******************************************************/
    static void
    HandleClubMissionFlowFlags (bool missionEnd)
    {
        if (Rainbomizer::Common::GetStoredEpisodeNumber () != 2)
            return;

        static int previousState = 0;
        if (!missionEnd)
            previousState = CTheScripts::m_pGlobals ()[MFF_CLUBS_SHUTDOWN];

        CTheScripts::m_pGlobals ()[MFF_CLUBS_SHUTDOWN]
            = (missionEnd) ? previousState : 0;
    }

    /*******************************************************/
    static void
    AdjustScriptCodeForNewStrand (const std::string &newThread,
                                  const std::string &originalThread)
    {
        scrProgram *program = CTheScripts::GetScrProgram (newThread);

        // Set the strand and mission hash for future reference
        mOriginalMission     = mMissionInfos[originalThread];
        mRandomizedMission   = mMissionInfos[newThread];
        mMissionHash         = program->m_dwHash;
        mOriginalMissionHash = CCrypto::atStringHash (originalThread.c_str ());

        HandleMissionStart ();

        AdjustMissionPassCode (program);
        AdjustMissionFailCode (program);
    }

    /*******************************************************/
    static void
    StartNewScript (NativeData *data)
    {
        bool adjustScript = false;

        std::string originalThread = data->Params[0].cstr_param;
        std::transform (originalThread.begin (), originalThread.end (),
                        originalThread.begin (),
                        [] (unsigned char c) { return std::tolower (c); });

        std::string newThread = GetNewMission (originalThread);

        if (mMissionMaps.count (originalThread))
            {
                InitialiseStoredSeed ();

                data->Params[0].cstr_param = newThread.data ();
                adjustScript               = true;

                // 8192 is the default (forced) stack size, so if the forced
                // stack size is set to 8192, use a greater stack size if the
                // mission requests.

                data->Params[1].int_param
                    = std::max (mMissionInfos[newThread].stackSize,
                                (mMissionInfos[newThread].stackSize == 8192)
                                    ? data->Params[1].int_param
                                    : 0);
            }

        CNativeManager::CallOriginalNative ("START_NEW_SCRIPT", data);

        // This is after calling the native because the native does some things
        // with the active scripts
        if (adjustScript)
            AdjustScriptCodeForNewStrand (newThread, originalThread);
    }

    /*******************************************************/
    static int
    GetPlayerId ()
    {
        return CNativeManager::CallNativeRet<int> ("GET_PLAYER_ID");
    }

    /*******************************************************/
    static int
    GetPlayerChar ()
    {
        int player = -1;
        CNativeManager::CallNative ("GET_PLAYER_CHAR", GetPlayerId (), &player);

        return player;
    }

    /*******************************************************/
    static MissionStrandInfo *
    GetMissionStrand (int val)
    {

        static auto globals = std::array{10982, 12307, 12383};
        static auto sizes   = std::array{84, 228, 229};
        int         episode = Rainbomizer::Common::GetStoredEpisodeNumber ();

        return reinterpret_cast<MissionStrandInfo *> (
            &CTheScripts::m_pGlobals ()[globals[episode]
                                        + sizes[episode] * val]);
    }

    /*******************************************************/
    static void
    ApplyMissionSpecificFixes ()
    {
        switch (mMissionHash)
            {
            // gerry1 - Actions Speak Louder Than Words
            // You need Gerry's contact to call him for the bomb
            case "gerry1"_joaat:
                CTheScripts::m_pGlobals ()[GERRY_CONTACT] = 1;
                break;

            // It's Your Call - mission takes away your phone / gives you an
            // old one on failing/passing respectively
            case "roman2"_joaat:
                mStoredMobilePhone
                    = CTheScripts::m_pGlobals ()[PLAYER_PHONE_MODEL];
                [[fallthrough]];

            // It's Your Call, Easy Fare, Jamaican Heat, Uncle Vlad
            // Sets Bohan and Algonquin safehouses as their state affects
            // the spawning of Roman's taxi
            case "roman5"_joaat:
            case "roman6"_joaat:
            case "roman7"_joaat:
                mStoredBohanHouseState
                    = CTheScripts::m_pGlobals ()[IS_BOHAN_SAFEHOUSE_OPEN];
                CTheScripts::m_pGlobals ()[IS_BOHAN_SAFEHOUSE_OPEN] = 0;
                break;

            case "roman11"_joaat:
                mStoredBohanHouseState
                    = CTheScripts::m_pGlobals ()[IS_BOHAN_SAFEHOUSE_OPEN];
                break;
            }

        HandleClubMissionFlowFlags (false);
        HandleRomansSorrowChanges (false);
    }

    /*******************************************************/
    static void
    ApplyMissionEndSpecificFixes (bool passed)
    {
        switch (mMissionHash)
            {
                // billy1, roman1, tony1 - First missions so fade out and never
                // fade back in

            case "billy1"_joaat:
            case "tony1"_joaat:
                if (!passed)
                    CNativeManager::CallNative ("DO_SCREEN_FADE_IN", 1000);
                break;

                // roman1 freezes you at the end to play the safehouse intro
                // scene
            case "roman1"_joaat:
                if (!passed)
                    CNativeManager::CallNative ("DO_SCREEN_FADE_IN", 1000);

                CNativeManager::CallNative ("SET_PLAYER_CONTROL",
                                            GetPlayerId (), 1);
                break;

            case "roman2"_joaat:
                // Restore  player's phone model since it's most likely gonna be
                // replaced by the mission
                CTheScripts::m_pGlobals ()[PLAYER_PHONE_MODEL]
                    = std::max (CTheScripts::m_pGlobals ()[PLAYER_PHONE_MODEL],
                                mStoredMobilePhone);

                [[fallthrough]];

            case "roman5"_joaat:
            case "roman6"_joaat:
            case "roman7"_joaat:
                CTheScripts::m_pGlobals ()[IS_BOHAN_SAFEHOUSE_OPEN]
                    = mStoredBohanHouseState;
                break;

            // Roman's Sorrow - Fix locked savehouses
            case "roman11"_joaat:
                if (mStoredBohanHouseState == 0
                    && mOriginalMissionHash != "roman11"_joaat)
                    {
                        static_assert ("cj_ext_door_17"_joaat == 0x820550A0);

                        // Enable Broker and disable Bohan savehouse
                        CNativeManager::CallNative (
                            "ENABLE_SAVE_HOUSE",
                            CTheScripts::m_pGlobals ()[BROKER_SAVEHOUSE_INDEX],
                            1);

                        CNativeManager::CallNative (
                            "ENABLE_SAVE_HOUSE",
                            CTheScripts::m_pGlobals ()[BOHAN_SAVEHOUSE_INDEX],
                            0);

                        CNativeManager::CallNative (
                            "SET_STATE_OF_CLOSEST_DOOR_OF_TYPE",
                            "cj_ext_door_17"_joaat, 896.0f, -504.0f, 15.0f, 0,
                            0.0f);

                        CTheScripts::m_pGlobals ()[ROMAN_APARTMENT_BURNED_DOWN]
                            = 0;

                        SetRomansApartmentState (false);
                    }
                break;

            // https://cdn.discordapp.com/attachments/682894128530456587/720002584533794816/Homeless.jpg
            // Fry fix :KappaYT:
            case "stubbs4"_joaat: SetClubhouseState (true); break;
            }

        /*******************************************************/
        switch (mOriginalMissionHash)
            {
                // Fade out if the first missions are failed
                // because some missions require fade-outs to have happened
                // before starting. Some missions do the fade-outs themselves,
                // others don't
            case "billy1"_joaat:
            case "tony1"_joaat:
            case "roman1"_joaat:
                if (!passed)
                    CNativeManager::CallNative ("DO_SCREEN_FADE_OUT", 1000);
                break;

            case "roman11"_joaat:
                SetRomansApartmentState (true);
                CNativeManager::CallNative ("SET_STATE_OF_CLOSEST_DOOR_OF_TYPE",
                                            "cj_ext_door_17"_joaat, 896.0f,
                                            -504.0f, 15.0f, 1, 0.0f);
                break;

            case "stubbs4"_joaat: SetClubhouseState (false); break;
            }

        HandleClubMissionFlowFlags (true);
        HandleRomansSorrowChanges (true);
    }

    /*******************************************************/
    static void
    TeleportPlayerImmediately (const Vector3 &dest)
    {
        CNativeManager::CallNative ("ALLOW_GAME_TO_PAUSE_FOR_STREAMING", true);

        CNativeManager::CallNative ("LOAD_SCENE", dest.x, dest.y, dest.z);

        CNativeManager::CallNative ("SET_CHAR_COORDINATES_NO_OFFSET",
                                    GetPlayerChar (), dest.x, dest.y, dest.z);
    }

    /*******************************************************/
    static void
    TeleportPlayerWithFades (const Vector3 &position, bool fadeIn = true)
    {
        CNativeManager::CallNative ("DO_SCREEN_FADE_OUT", 1000);

        mbFading              = true;
        miFadeTimer           = time (NULL);
        mbFadeInAfterTeleport = fadeIn;
        mvPosAfterFade        = position;
    }

    /*******************************************************/
    static void
    HandleMissionStart ()
    {
        // Set the number of unlocked cities to lock/unlock cities
        if (mRandomizedMission.citiesUnlocked > 0)
            {
                CNativeManager::CallNativeRet (&mStoredIslandsUnlocked,
                                               "GET_INT_STAT",
                                               STAT_CITIES_UNLOCKED);

                CNativeManager::CallNative ("SET_INT_STAT",
                                            STAT_CITIES_UNLOCKED,
                                            mRandomizedMission.citiesUnlocked);
            }

        if (mOriginalMission.phoneMission)
            TeleportPlayerWithFades (mRandomizedMission.startPos,
                                     mRandomizedMission.phoneMission);

        else
            TeleportPlayerImmediately (mRandomizedMission.startPos);

        if (mRandomizedMission.phoneMission && !mOriginalMission.phoneMission)
            {
                CNativeManager::CallNative ("DO_SCREEN_FADE_IN", 1000);
                CNativeManager::CallNative ("SET_PLAYER_CONTROL",
                                            GetPlayerId (), 1);
            }

        // Give the player a phone if they don't have one (GTAIV only)
        if (Rainbomizer::Common::GetStoredEpisodeNumber () == 0)
            CTheScripts::m_pGlobals ()[PLAYER_PHONE_MODEL]
                = std::max (CTheScripts::m_pGlobals ()[PLAYER_PHONE_MODEL], 1);

        ApplyMissionSpecificFixes ();
    }

    /*******************************************************/
    static void
    HandleMissionPass ()
    {
        TeleportPlayerWithFades (mOriginalMission.endPos);
    }

    /*******************************************************/
    static void
    HandleMissionEnd ()
    {
        if (mRandomizedMission.citiesUnlocked == -1)
            return;

        int newCitiesUnlocked = mStoredIslandsUnlocked;

        if (mOriginalMission.citiesUnlockedStart != -1)
            newCitiesUnlocked = mOriginalMission.citiesUnlockedStart;

        CNativeManager::CallNative ("SET_INT_STAT", STAT_CITIES_UNLOCKED,
                                    newCitiesUnlocked);
    }

    /*******************************************************/
    // Waits for the game to fade out completely after a mission pass. This is
    // intended to stop the script execution whilst the game fades to prevent
    // any wanted level issues and make the game think that you ended the
    // mission at the end position
    /*******************************************************/
    static bool
    HandleFadingCode (scrThread *scr)
    {
        const int FADE_TIMEOUT = 11;

        if (!mbFading)
            return false;

        if (!CNativeManager::CallNativeRet<bool> ("IS_SCREEN_FADED_OUT")
            && time (NULL) - miFadeTimer < FADE_TIMEOUT)
            return true;

        TeleportPlayerImmediately (mvPosAfterFade);
        if (mbFadeInAfterTeleport)
            CNativeManager::CallNative ("DO_SCREEN_FADE_IN", 1000);

        mbFading = false;
        return false;
    }

    /*******************************************************/
    static void
    HandleMissionState (scrThread *scr)
    {
        if (mMissionHash == -1 || scr->m_context.dwScriptHash != mMissionHash)
            return;

        auto strand = GetMissionStrand (mOriginalMission.strand);

        // Thread ended
        if (scr->m_context.eThreadState == 2
            || strand->m_nLastMissionPassed != -1)
            {
                bool passed = strand->m_nLastMissionPassed != -1;

                // And the mission was passed
                if (passed)
                    HandleMissionPass ();

                ApplyMissionEndSpecificFixes (passed);
                HandleMissionEnd ();
                mMissionHash = -1;
            }
    }

    /*******************************************************/
    static int __fastcall RunThreadHook (scrThread *scr, void *edx,
                                         unsigned int param_2)
    {
        if (HandleFadingCode (scr))
            return 0;

        int ret = scrThread__RunOriginal (scr, param_2);
        return (HandleMissionState (scr), ret);
    }

    /*******************************************************/
    void
    InitialiseVFTableHooks ()
    {
        scrThread__vftable *gtaThread_vftable
            = *hook::get_pattern<scrThread__vftable *> (
                "c7 86 a8 00 00 00 00 00 00 00 8b c6 5e c3", -9);

        scrThread__vftable *scrThread_vftable
            = *hook::get_pattern<scrThread__vftable *> (
                ByVersion ("68 04 20 00 00 c7 06 ? ? ? ? 89 6e 58",
                           "8b 30 57 8b f9 6a 00 c7 07"),
                ByVersion (7, 9));

        // Unprotect memory because DEP
        DWORD oldProtect;
        injector::UnprotectMemory (gtaThread_vftable,
                                   sizeof (scrThread__vftable), oldProtect);
        injector::UnprotectMemory (scrThread_vftable,
                                   sizeof (scrThread__vftable), oldProtect);

        scrThread__RunOriginal = gtaThread_vftable->Run;

        ConvertCall (RunThreadHook, gtaThread_vftable->Run);
        ConvertCall (RunThreadHook, scrThread_vftable->Run);
    }

    /*******************************************************/
    static void
    InitialiseMissionsMap (unsigned int seed)
    {
        // Inefficient but whatever
        std::vector<std::string> missions;
        std::vector<std::string> order;
        std::mt19937             engine{seed};

        for (const auto &s : mMissionInfos)
            missions.push_back (s.first);

        order = missions;
        std::shuffle (order.begin (), order.end (), engine);

        for (int i = 0; i < missions.size (); i++)
            mMissionMaps[missions[i]] = order[i];

        mCurrentMissionSeed = seed;
    }

    /*******************************************************/
    static bool
    ReadMissionsFile (const std::string &name)
    {
        FILE *file = Rainbomizer::Common::GetRainbomizerDataFile (name);
        mMissionInfos.clear ();

        if (!file)
            return false;

        char line[2048] = {0};
        while (fgets (line, 2048, file))
            {
                if (line[0] == '#' || strlen (line) < 10)
                    continue;

                char    thread[32] = {0};
                Vector3 pos;
                int     strand;
                int     citiesUnlocked      = -1;
                char    phoneMission        = 'N';
                int     citiesUnlockedStart = -1;
                Vector3 posStarting         = {842.122f, 935.903f, 1.861f};
                int     stackSize           = 8192;

                sscanf (line,
                        "%s %*s %d %f %f %f %d %d %c %f %f %f %*c %*f %*f "
                        "%*f %d",
                        thread, &strand, &pos.x, &pos.y, &pos.z,
                        &citiesUnlocked, &citiesUnlockedStart, &phoneMission,
                        &posStarting.x, &posStarting.y, &posStarting.z,
                        &stackSize);

                mMissionInfos[thread] = {strand,
                                         pos,
                                         citiesUnlocked,
                                         citiesUnlockedStart,
                                         phoneMission == 'Y',
                                         posStarting,
                                         stackSize};
            }

        return true;
    }
    
public:
    /*******************************************************/
    MissionRandomizer ()
    {
        if (!ConfigManager::GetConfigs ().missions.enabled)
            return;

        InitialiseAllComponents ();

        Rainbomizer::Common::AddEpisodeChangeCallback ([] (int) {
            ReadMissionsFile ("Missions.txt");

            int seed = ConfigManager::GetConfigs ().missions.seed;
            if (seed == -1)
                seed = time (NULL);

            InitialiseMissionsMap (seed);
        });

        InitialiseVFTableHooks ();

        CNativeManager::OverwriteNative ("HAS_SCRIPT_LOADED", HasScriptLoaded);
        CNativeManager::OverwriteNative ("REQUEST_SCRIPT", RequestScript);
        CNativeManager::OverwriteNative ("MARK_SCRIPT_AS_NO_LONGER_NEEDED",
                                         MarkScriptAsNoLongerNeeded);
        CNativeManager::OverwriteNative ("START_NEW_SCRIPT", StartNewScript);

        Rainbomizer::Logger::LogMessage ("Initialised MissionRandomizer");
    }
};

std::unordered_map<std::string, std::string> MissionRandomizer::mMissionMaps{};
MissionInfo    MissionRandomizer::mRandomizedMission;
MissionInfo    MissionRandomizer::mOriginalMission;
uint32_t       MissionRandomizer::mMissionHash           = -1;
uint32_t       MissionRandomizer::mOriginalMissionHash   = -1;
int            MissionRandomizer::mStoredIslandsUnlocked = -1;
bool           MissionRandomizer::mbFading               = false;
bool           MissionRandomizer::mbFadeInAfterTeleport  = true;
int            MissionRandomizer::miFadeTimer            = 0;
PreviousChange MissionRandomizer::mPreviousChange;
int            MissionRandomizer::mStoredBohanHouseState = 0;
Vector3        MissionRandomizer::mvPosAfterFade;
int            MissionRandomizer::mStoredMobilePhone = 0;
uint32_t       MissionRandomizer::mCurrentMissionSeed = 0;

MissionRandomizer _missions;
