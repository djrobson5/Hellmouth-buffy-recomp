#include "app/app.h"

void WidescreenInit();  // game/widescreen.cpp

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Buffy the Vampire Slayer",
    .short_title = L"Buffy",
    .name = L"Buffy",
    .log = "buffy",
    .settings = "settings.toml",
    .version = "buffy-port 0.1.1",
    .splash = L"splash",
    .missing = L"The game files couldn't be found. Give the game's disc image (.iso), or a folder with Default.xbe and its files.",
    .title_id = 0x45410012,
    .images = {{"default.xbe", "Buffy"}},  // lower case: the launch path is compared in lower case
    .init = WidescreenInit,
    // TInputDeviceController::Poll (0x244F0), both sticks: centred inside 13107.2 of 32768, then rising to full at
    // 13107.2 + 18204.44 (1 / its scale, 0x618C74)
    .stick_deadzone = 13107.2f / 32768,
    .stick_full = (13107.2f + 18204.44f) / 32768,
    // what earns each is in game/achievements.cpp. The ids are permanent once released. Descriptions name places, not bosses:
    // the menu's page lists them all from the start.
    .achievements = {
        {"ch_training", "Welcome to the Hellmouth", "Finish the Spanish Mission."},
        {"boss_malik", "The Harvest", "Beat the High School's boss."},
        {"boss_spike", "School Hard", "Beat The Bronze's boss."},
        {"boss_scylla", "Bad Girls", "Beat the Cemetery's boss."},
        {"boss_matereani", "Dead Man's Party", "Beat the Sunken Church's boss."},
        {"boss_scylla_snake", "Reptile Boy", "Beat the Mansion's boss."},
        {"boss_urd", "Go Fish", "Beat the Docks' boss."},
        {"boss_malik_tiger", "Homecoming", "Beat the boss of the High School's return."},
        {"boss_angel", "Becoming", "Beat the Foundry's boss."},
        {"boss_dark_buffy", "Doppelgangland", "Beat the first boss of the Dreamers' Realm."},
        {"boss_dreamers", "Restless", "Beat the last boss of the Dreamers' Realm."},
        {"boss_master", "Prophecy Girl", "Beat the final boss."},
        {"game_finished", "She Saved the World. A Lot.", "Finish the game."},
        {"game_hard", "Five by Five", "Finish the game on Hard."},
        {"kills_1", "Into Every Generation", "Kill your first enemy."},
        {"kills_100", "Mr. Pointy", "Kill 100 enemies."},
        {"kills_250", "Bored Now", "Kill 250 enemies."},
        {"kills_500", "The Slayer", "Kill 500 enemies."},
        {"secret_1", "Research Party", "Find a secret."},
        {"secret_level", "What's My Line?", "Find every secret in a level with at least 3."},
        {"secret_30", "The Watcher's Diary", "Find 30 secrets."},
        {"secret_all", "Giles Would Be Proud", "Find all 77 secrets."},
        {"die_once", "Killed by Death", "Die for the first time."},
        {"chapter_deathless", "Not Today", "Go from a new game or a gang meeting in the Library to the next meeting (or the end) without dying."},
        {"history", "Teacher's Pet", "Finish Buffy History 101 in Extras."},
    },
};
